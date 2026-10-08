/**
 * C++ port of CAVA's cavacore.c algorithm.
 * See: https://github.com/karlstav/cava/blob/master/cavacore.c
 *
 * Existing improvements:
 *  - HIGH_CUT_OFF raised to 20 kHz; runtime-configurable via setHighCutoff()
 *  - gravity_factor_: runtime fall-speed multiplier (1.0 = CAVA default)
 *  - mcat_factor_: runtime monstercat strength (1.5 = CAVA default, 0 = off)
 *  - rise_factor_: attack smoothing applied after sensitivity
 *  - reinit(ch): reinitialise in-place without moving the object
 *  - Fixed OOB: cava_out_[n+num_bars] write in mono mode
 *  - Hann windows built once at construction
 *  - FFTW_ESTIMATE used in reinit() for instant stereo toggle
 *  - EMA framerate window 64→20 for faster FPS tracking
 *  - FPS-normalised gravity fall step and integral decay
 *  - O(n) monstercat: two-pass rolling-max
 *  - Auto-sens: starts at 1/man_sens_, init ramp 0.1→0.04
 *
 * New additions:
 *  - A-weighting (IEC 61672): perceptual per-bar frequency weighting
 *  - Noise floor gate: bars below threshold clamped to zero
 *  - Per-bar smoothing: bass bars get heavier decay + slower fall
 *  - Stereo correlation: EMA-smoothed L/R correlation; auto-mono above
 * threshold
 */
#include "fft_processor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ── A-weighting (IEC 61672)
// ─────────────────────────────────────────────────── Returns the linear
// A-weight factor for a given frequency, normalised so that aWeightFactor(1000
// Hz) == 1.0. Values: ~0.01 at 50 Hz, ~0.1 at 100 Hz, ~1.0 at 1–4 kHz, ~0.5 at
// 10 kHz.
double FFTProcessor::aWeightFactor(double freq) noexcept {
  if (freq <= 0.0)
    return 1.0;
  const double f2 = freq * freq;
  const double f4 = f2 * f2;
  // Ra(f) formula from IEC 61672
  const double ra = (12200.0 * 12200.0 * f4) /
                    ((f2 + 20.6 * 20.6) *
                     std::sqrt((f2 + 107.7 * 107.7) * (f2 + 737.9 * 737.9)) *
                     (f2 + 12200.0 * 12200.0));
  // Normalise to Ra(1000 Hz)
  static const double kRef = [] {
    const double f2k = 1000.0 * 1000.0, f4k = f2k * f2k;
    return (12200.0 * 12200.0 * f4k) /
           ((f2k + 20.6 * 20.6) *
            std::sqrt((f2k + 107.7 * 107.7) * (f2k + 737.9 * 737.9)) *
            (f2k + 12200.0 * 12200.0));
  }();
  return ra / kRef;
}

// ─────────────────────────────────────────────────────────────────────────────
void FFTProcessor::initBufferSizes() {
  // CAVA lookup table (cavacore.c lines 34-46)
  int base = 512;
  if (rate_ > 8125 && rate_ <= 16250)
    base *= 2;
  else if (rate_ > 16250 && rate_ <= 32500)
    base *= 4;
  else if (rate_ > 32500 && rate_ <= 75000)
    base *= 8; // 44100 → 4096
  else if (rate_ > 75000 && rate_ <= 150000)
    base *= 16;
  else if (rate_ > 150000 && rate_ <= 300000)
    base *= 32;
  else if (rate_ > 300000)
    base *= 64;
  fft_buf_size_ = base;
  bass_buf_size_ = base * 2;
}

FFTProcessor::FFTProcessor(int sr, int ch) : rate_(sr), channels_(ch) {
  initBufferSizes();
  input_buf_size_ = bass_buf_size_ * channels_;
  input_buf_.assign(input_buf_size_, 0.0);
  allocRing();
  buildHannWindows();
  // Initialise auto-sens multiplier so combined_sens = sens_ * man_sens_ = 1.0
  // from the very first frame, preventing the initial blast.
  sens_ = 1.0 / (double)man_sens_.load();
  initFFTW(false); // FFTW_MEASURE at startup — best runtime performance
}

FFTProcessor::~FFTProcessor() { freeFFTW(); }

// ── reinit: change channel count without moving the object ───────────────────
void FFTProcessor::reinit(int new_channels) {
  // Contract: capture is stopped, so no producer is running.
  channels_ = new_channels;
  input_buf_size_ = bass_buf_size_ * channels_;
  input_buf_.assign(input_buf_size_, 0.0);
  allocRing();

  freeFFTW();
  initFFTW(
      true); // FFTW_ESTIMATE — instant; imperceptible perf difference at 60fps

  num_bars_ = 0;
  cava_out_.clear();
  cava_peak_.clear();
  cava_fall_.clear();
  cava_mem_.clear();
  prev_out_.clear();
  bars_l_.clear();
  bars_r_.clear();

  sens_ = 1.0 / (double)man_sens_.load();
  sens_init_ = true;
  framerate_ = 60.0;
  frame_skip_ = 1;
  corr_ema_ = 1.0;
  auto_mono_collapsed_ = false;
}

// ── Hann windows
// ──────────────────────────────────────────────────────────────
void FFTProcessor::buildHannWindows() {
  bass_win_.resize(bass_buf_size_);
  mid_win_.resize(fft_buf_size_);
  for (int i = 0; i < bass_buf_size_; ++i)
    bass_win_[i] =
        0.5 * (1.0 - std::cos(2.0 * M_PI * i / (bass_buf_size_ - 1)));
  for (int i = 0; i < fft_buf_size_; ++i)
    mid_win_[i] = 0.5 * (1.0 - std::cos(2.0 * M_PI * i / (fft_buf_size_ - 1)));
}

// ── FFTW allocation
// ───────────────────────────────────────────────────────────
void FFTProcessor::initFFTW(bool fast) {
  const unsigned flags = fast ? FFTW_ESTIMATE : FFTW_MEASURE;

  in_bass_l_ = fftw_alloc_real(bass_buf_size_);
  in_mid_l_ = fftw_alloc_real(fft_buf_size_);
  out_bass_l_ = fftw_alloc_complex(bass_buf_size_ / 2 + 1);
  out_mid_l_ = fftw_alloc_complex(fft_buf_size_ / 2 + 1);
  if (!in_bass_l_ || !in_mid_l_ || !out_bass_l_ || !out_mid_l_)
    throw std::runtime_error("FFTProcessor: FFTW alloc failed (L)");

  plan_bass_l_ =
      fftw_plan_dft_r2c_1d(bass_buf_size_, in_bass_l_, out_bass_l_, flags);
  plan_mid_l_ =
      fftw_plan_dft_r2c_1d(fft_buf_size_, in_mid_l_, out_mid_l_, flags);

  if (channels_ == 2) {
    in_bass_r_ = fftw_alloc_real(bass_buf_size_);
    in_mid_r_ = fftw_alloc_real(fft_buf_size_);
    out_bass_r_ = fftw_alloc_complex(bass_buf_size_ / 2 + 1);
    out_mid_r_ = fftw_alloc_complex(fft_buf_size_ / 2 + 1);
    if (!in_bass_r_ || !in_mid_r_ || !out_bass_r_ || !out_mid_r_)
      throw std::runtime_error("FFTProcessor: FFTW alloc failed (R)");
    plan_bass_r_ =
        fftw_plan_dft_r2c_1d(bass_buf_size_, in_bass_r_, out_bass_r_, flags);
    plan_mid_r_ =
        fftw_plan_dft_r2c_1d(fft_buf_size_, in_mid_r_, out_mid_r_, flags);
  }

  std::memset(in_bass_l_, 0, sizeof(double) * bass_buf_size_);
  std::memset(in_mid_l_, 0, sizeof(double) * fft_buf_size_);
}

void FFTProcessor::freeFFTW() {
  if (plan_bass_l_) {
    fftw_destroy_plan(plan_bass_l_);
    plan_bass_l_ = nullptr;
  }
  if (plan_mid_l_) {
    fftw_destroy_plan(plan_mid_l_);
    plan_mid_l_ = nullptr;
  }
  if (plan_bass_r_) {
    fftw_destroy_plan(plan_bass_r_);
    plan_bass_r_ = nullptr;
  }
  if (plan_mid_r_) {
    fftw_destroy_plan(plan_mid_r_);
    plan_mid_r_ = nullptr;
  }
  if (out_bass_l_) {
    fftw_free(out_bass_l_);
    out_bass_l_ = nullptr;
  }
  if (out_mid_l_) {
    fftw_free(out_mid_l_);
    out_mid_l_ = nullptr;
  }
  if (out_bass_r_) {
    fftw_free(out_bass_r_);
    out_bass_r_ = nullptr;
  }
  if (out_mid_r_) {
    fftw_free(out_mid_r_);
    out_mid_r_ = nullptr;
  }
  if (in_bass_l_) {
    fftw_free(in_bass_l_);
    in_bass_l_ = nullptr;
  }
  if (in_mid_l_) {
    fftw_free(in_mid_l_);
    in_mid_l_ = nullptr;
  }
  if (in_bass_r_) {
    fftw_free(in_bass_r_);
    in_bass_r_ = nullptr;
  }
  if (in_mid_r_) {
    fftw_free(in_mid_r_);
    in_mid_r_ = nullptr;
  }
}

// ── Frequency plan
// ────────────────────────────────────────────────────────────
void FFTProcessor::buildPlan(int num_bars) {
  num_bars_ = num_bars;

  cut_lo_.resize(num_bars + 1);
  cut_hi_.resize(num_bars + 1);
  eq_.resize(num_bars);
  aw_.resize(num_bars, 1.0);

  // Always allocate 2*num_bars so both L and R halves are in-bounds.
  const int total = num_bars * 2;
  cava_out_.assign(total, 0.0);
  cava_peak_.assign(total, 0.0);
  cava_fall_.assign(total, 0.0);
  cava_mem_.assign(total, 0.0);
  prev_out_.assign(total, 0.0);
  bars_l_.assign(num_bars, 0.f);
  bars_r_.assign(num_bars, 0.f);

  const int lo = LOW_CUT_OFF;
  const int hi = high_cutoff_;
  double fc = std::log10((double)lo / hi) / (1.0 / (num_bars + 1) - 1.0);
  const float min_bw = (float)rate_ / bass_buf_size_;

  std::vector<float> cut_freq(num_bars + 1);
  bass_cut_bar_ = 0;
  bool first_bar = true;

  for (int n = 0; n <= num_bars; ++n) {
    double coeff = fc * (-1.0) + (double)(n + 1) / (num_bars + 1) * fc;
    cut_freq[n] = (float)(hi * std::pow(10.0, coeff));

    if (n > 0 && cut_freq[n - 1] >= cut_freq[n])
      cut_freq[n] = cut_freq[n - 1] + min_bw;

    float rel = cut_freq[n] / ((float)rate_ / 2.0f);

    if (cut_freq[n] < BASS_CUT_OFF) {
      cut_lo_[n] = (int)(rel * (bass_buf_size_ / 2));
      cut_lo_[n] = std::min(cut_lo_[n], bass_buf_size_ / 2);
      bass_cut_bar_++;
      if (bass_cut_bar_ > 1)
        first_bar = false;
    } else {
      cut_lo_[n] = (int)std::ceil(rel * (float)(fft_buf_size_ / 2));
      if (n == bass_cut_bar_) {
        first_bar = true;
        if (n > 0)
          cut_hi_[n - 1] = (int)(rel * (bass_buf_size_ / 2)) - 1;
      } else {
        first_bar = false;
      }
      cut_lo_[n] = std::min(cut_lo_[n], fft_buf_size_ / 2);
    }

    if (n > 0) {
      if (!first_bar) {
        cut_hi_[n - 1] = cut_lo_[n] - 1;
        if (cut_lo_[n] <= cut_lo_[n - 1]) {
          int maxbin =
              (n < bass_cut_bar_) ? bass_buf_size_ / 2 : fft_buf_size_ / 2;
          if (cut_lo_[n - 1] + 1 < maxbin + 1) {
            cut_lo_[n] = cut_lo_[n - 1] + 1;
            cut_hi_[n - 1] = cut_lo_[n] - 1;
          }
        }
      } else {
        if (cut_hi_[n - 1] < cut_lo_[n - 1])
          cut_hi_[n - 1] = cut_lo_[n - 1] + 1;
      }
    }

    if (n < bass_cut_bar_)
      cut_freq[n] = (float)cut_lo_[n] / (float)(bass_buf_size_ / 2) *
                    ((float)rate_ / 2.0f);
    else
      cut_freq[n] = (float)cut_lo_[n] / (float)(fft_buf_size_ / 2) *
                    ((float)rate_ / 2.0f);
  }

  // EQ per bar — CAVA formula
  for (int n = 0; n < num_bars; ++n) {
    eq_[n] = 1.0 / std::pow(2.0, 28.0);
    eq_[n] *= std::pow((double)cut_freq[n + 1], 0.85);
    eq_[n] /= (n < bass_cut_bar_) ? std::log2((double)bass_buf_size_)
                                  : std::log2((double)fft_buf_size_);
    int span = cut_hi_[n] - cut_lo_[n] + 1;
    if (span < 1)
      span = 1;
    eq_[n] /= span;

    // A-weighting factor — computed unconditionally so toggle is instant.
    // Centre frequency: use geometric mean of the bar's boundary frequencies.
    double fc_hz = std::sqrt((double)cut_freq[n] * (double)cut_freq[n + 1]);
    aw_[n] = aWeightFactor(std::max(fc_hz, 1.0));
  }
}

// ── Ring buffer + addSamples (audio thread) ─────────────────────────────────
void FFTProcessor::allocRing() {
  ring_size_ = static_cast<std::size_t>(input_buf_size_) * 4;
  ring_ = std::make_unique<std::atomic<float>[]>(ring_size_);
  for (std::size_t i = 0; i < ring_size_; ++i)
    ring_[i].store(0.0f, std::memory_order_relaxed);
  wcount_.store(0, std::memory_order_relaxed);
  rcount_ = 0;
}

void FFTProcessor::addSamples(const float *s, std::size_t count, int ch) {
  if (!s || count == 0 || ring_size_ == 0 || ch < 1)
    return;

  std::uint64_t w =
      wcount_.load(std::memory_order_relaxed); // we're the only writer
  std::size_t idx = static_cast<std::size_t>(w % ring_size_);
  const auto put = [&](float v) {
    ring_[idx].store(v, std::memory_order_relaxed);
    if (++idx == ring_size_)
      idx = 0;
    ++w;
  };

  const int pc = channels_;
  if (ch == pc) {
    // Fast path.  If a single chunk exceeds the ring only its tail matters.
    const std::size_t skip = count > ring_size_ ? count - ring_size_ : 0;
    w += skip;
    idx = (idx + skip) % ring_size_;
    for (std::size_t i = skip; i < count; ++i)
      put(s[i]);
  } else {
    // Layout differs from what the processor was built for (e.g. the
    // server negotiated mono): convert instead of misreading the data.
    const std::size_t frames = count / static_cast<std::size_t>(ch);
    for (std::size_t f = 0; f < frames; ++f) {
      const float *fr = s + f * static_cast<std::size_t>(ch);
      if (pc == 2) {
        put(fr[0]);
        put(ch >= 2 ? fr[1] : fr[0]);
      } else { // pc == 1
        put(ch >= 2 ? 0.5f * (fr[0] + fr[1]) : fr[0]);
      }
    }
  }
  wcount_.store(w, std::memory_order_release);
}

// ── O(n) monstercat: two-pass rolling-max ────────────────────────────────────
void FFTProcessor::applyMonstercat(std::vector<double> &bars,
                                   double factor) const {
  const int n = (int)bars.size();
  if (n < 2 || factor <= 0.0)
    return;
  // Defence in depth: decay must stay > 1 or the rolling max amplifies.
  const double decay = std::max(factor, 1.0) * 1.5;
  for (int i = 1; i < n; ++i)
    bars[i] = std::max(bars[i], bars[i - 1] / decay);
  for (int i = n - 2; i >= 0; --i)
    bars[i] = std::max(bars[i], bars[i + 1] / decay);
}

// ── execute
// ───────────────────────────────────────────────────────────────────
bool FFTProcessor::execute(int num_bars, float /*fps*/) {
  if (num_bars <= 0)
    return false;

  if (num_bars != num_bars_)
    buildPlan(num_bars);

  const std::uint64_t total = wcount_.load(std::memory_order_acquire);
  const std::uint64_t delta = total - rcount_;
  rcount_ = total;
  const int new_samples =
      static_cast<int>(std::min<std::uint64_t>(delta, 1u << 30));
  bool silence = true;

  if (new_samples > 0) {
    // EMA window of 20 (was CAVA's 64) — tracks FPS changes ~3x faster.
    framerate_ -= framerate_ / 20.0;
    framerate_ += (double)(rate_ * frame_skip_) /
                  ((double)new_samples / channels_) / 20.0;
    frame_skip_ = 1;

    {
      // Newest sample first.  fill <= delta <= total, so total-1-n >= 0.
      const int fill = std::min(new_samples, input_buf_size_);
      std::memmove(input_buf_.data() + fill, input_buf_.data(),
                   static_cast<std::size_t>(input_buf_size_ - fill) *
                       sizeof(double));
      for (int n = 0; n < fill; ++n) {
        const std::size_t idx = static_cast<std::size_t>(
            (total - 1 - static_cast<std::uint64_t>(n)) % ring_size_);
        const double v =
            static_cast<double>(ring_[idx].load(std::memory_order_relaxed)) *
            32768.0;
        input_buf_[n] = v;
        if (std::fabs(v) > SILENCE_PEAK)
          silence = false;
      }
    }
  } else {
    frame_skip_++;
  }

  // ── Deinterleave ─────────────────────────────────────────────────────────
  if (channels_ == 2) {
    for (int n = 0; n < bass_buf_size_; ++n) {
      in_bass_r_[n] = input_buf_[n * 2];
      in_bass_l_[n] = input_buf_[n * 2 + 1];
    }
    for (int n = 0; n < fft_buf_size_; ++n) {
      in_mid_r_[n] = input_buf_[n * 2];
      in_mid_l_[n] = input_buf_[n * 2 + 1];
    }
  } else {
    for (int n = 0; n < bass_buf_size_; ++n)
      in_bass_l_[n] = input_buf_[n];
    for (int n = 0; n < fft_buf_size_; ++n)
      in_mid_l_[n] = input_buf_[n];
  }

  // ── Hann window ───────────────────────────────────────────────────────────
  for (int i = 0; i < bass_buf_size_; ++i) {
    in_bass_l_[i] *= bass_win_[i];
    if (channels_ == 2)
      in_bass_r_[i] *= bass_win_[i];
  }
  for (int i = 0; i < fft_buf_size_; ++i) {
    in_mid_l_[i] *= mid_win_[i];
    if (channels_ == 2)
      in_mid_r_[i] *= mid_win_[i];
  }

  // ── FFTs ──────────────────────────────────────────────────────────────────
  fftw_execute(plan_bass_l_);
  fftw_execute(plan_mid_l_);
  if (channels_ == 2) {
    fftw_execute(plan_bass_r_);
    fftw_execute(plan_mid_r_);
  }

  // ── Bin → bar magnitude ───────────────────────────────────────────────────
  for (int n = 0; n < num_bars; ++n) {
    double temp_l = 0.0, temp_r = 0.0;
    for (int i = cut_lo_[n]; i <= cut_hi_[n]; ++i) {
      if (n < bass_cut_bar_) {
        temp_l += std::hypot(out_bass_l_[i][0], out_bass_l_[i][1]);
        if (channels_ == 2)
          temp_r += std::hypot(out_bass_r_[i][0], out_bass_r_[i][1]);
      } else {
        temp_l += std::hypot(out_mid_l_[i][0], out_mid_l_[i][1]);
        if (channels_ == 2)
          temp_r += std::hypot(out_mid_r_[i][0], out_mid_r_[i][1]);
      }
    }
    cava_out_[n] = temp_l * eq_[n];
    if (channels_ == 2)
      cava_out_[n + num_bars] = temp_r * eq_[n];
  }

  // ── A-weighting ───────────────────────────────────────────────────────────
  // Applied after bin→magnitude and EQ so it scales the final bar value by
  // the perceptual weight of the bar's centre frequency.
  if (a_weighting_) {
    for (int n = 0; n < num_bars; ++n) {
      cava_out_[n] *= aw_[n];
      if (channels_ == 2)
        cava_out_[n + num_bars] *= aw_[n];
    }
  }

  // ── Stereo correlation ────────────────────────────────────────────────────
  // Normalised cross-correlation of the (Hann-windowed) L and R WAVEFORMS.
  // (The previous version correlated bar MAGNITUDES.  Those are always
  // positive and share the same spectral tilt, so two completely unrelated
  // channels still scored ~1.0 and auto-mono collapsed genuine stereo.)
  //   1.0  = identical (true mono)     0.0 = unrelated     -1.0 = inverted
  // EMA-smoothed so a single odd frame can't flip the state.  On silence the
  // correlation is undefined, so the EMA simply holds.
  // When auto_mono_ is enabled and corr_ema_ exceeds the ON threshold,
  // barsR is set equal to barsL in the output section below.
  if (channels_ == 2) {
    double sum_ll = 0.0, sum_rr = 0.0, sum_lr = 0.0;
    for (int i = 0; i < fft_buf_size_; ++i) {
      const double l = in_mid_l_[i];
      const double r = in_mid_r_[i];
      sum_ll += l * l;
      sum_rr += r * r;
      sum_lr += l * r;
    }
    const double denom = std::sqrt(sum_ll * sum_rr);
    if (denom > 1.0) {
      const double corr = std::clamp(sum_lr / denom, -1.0, 1.0);
      corr_ema_ = corr_ema_ * (1.0 - CORR_EMA_K) + corr * CORR_EMA_K;
    }

    if (auto_mono_) {
      if (!auto_mono_collapsed_ && corr_ema_ >= MONO_CORR_ON)
        auto_mono_collapsed_ = true;
      else if (auto_mono_collapsed_ && corr_ema_ < MONO_CORR_OFF)
        auto_mono_collapsed_ = false;
    } else {
      auto_mono_collapsed_ = false;
    }
  } else {
    corr_ema_ = 1.0;
    auto_mono_collapsed_ = false;
  }

  // ── Sensitivity ───────────────────────────────────────────────────────────
  const double combined_sens = sens_ * (double)man_sens_.load();
  for (int n = 0; n < num_bars; ++n)
    cava_out_[n] *= combined_sens;
  if (channels_ == 2)
    for (int n = num_bars; n < num_bars * 2; ++n)
      cava_out_[n] *= combined_sens;

  // ── Noise floor gate ──────────────────────────────────────────────────────
  // Clamp bars below the gate threshold to zero after sensitivity is applied.
  // This eliminates the idle shimmer visible during silence or very quiet
  // audio.
  if (noise_gate_ > 0.0f) {
    const int gate_count = num_bars * (channels_ == 2 ? 2 : 1);
    for (int n = 0; n < gate_count; ++n)
      if (cava_out_[n] < (double)noise_gate_)
        cava_out_[n] = 0.0;
  }

  // ── Smoothing ─────────────────────────────────────────────────────────────
  //
  // FPS-normalised constants:
  //   fall_step = 0.028 * fr_mod   → fall duration invariant at any FPS
  //   decay_frame = NOISE_REDUCTION^(fps/66) → integral time-constant invariant
  //   at any FPS
  //
  // Per-bar smoothing (bass_smooth_):
  //   Bass bars (n=0) get extra integral smoothing and slower fall.
  //   The boost tapers linearly to zero at the highest-frequency bar.
  //   bar_frac = n_bar / (num_bars-1):  0 = bass,  1 = treble
  //   bass_decay = decay_frame + (1-decay_frame) * bass_smooth_ * (1-bar_frac)
  //   bass_fall  = fall_step  / (1 + bass_smooth_ * 2 * (1-bar_frac))
  //
  // When bass_smooth_ == 0 all bars use identical constants (original CAVA).

  const double fps_d = std::max(framerate_, 1.0);
  const double fr_mod = 66.0 / fps_d;
  const double gravity_mod = 2.0 / NOISE_REDUCTION * (double)gravity_factor_;
  const double fall_step = 0.028 * fr_mod;
  const double decay_frame = std::pow(NOISE_REDUCTION, fps_d / 66.0);

  int overshoot = 0;
  const int smooth_count = num_bars * (channels_ == 2 ? 2 : 1);

  for (int n = 0; n < smooth_count; ++n) {

    // Per-bar smoothing weight: 1 at bass, 0 at treble.
    const int n_bar = n % num_bars;
    const double bar_frac =
        (num_bars > 1) ? (double)n_bar / (double)(num_bars - 1) : 1.0;
    const double bass_w = (double)bass_smooth_ * (1.0 - bar_frac);

    const double bar_decay =
        std::min(0.99, decay_frame + (1.0 - decay_frame) * bass_w);
    // Bass bars fall more slowly so transients read more naturally.
    const double bar_fall = fall_step / std::max(0.1, 1.0 + bass_w * 2.0);

    // ── Rise smoothing (applied after sensitivity) ────────────────────────
    if (rise_factor_ > 0.0f && cava_out_[n] > prev_out_[n]) {
      cava_out_[n] = prev_out_[n] + (cava_out_[n] - prev_out_[n]) *
                                        (1.0 - (double)rise_factor_);
    }

    // ── Gravity fall ──────────────────────────────────────────────────────
    if (cava_out_[n] < prev_out_[n] && NOISE_REDUCTION > 0.1) {
      cava_out_[n] =
          cava_peak_[n] * (1.0 - (cava_fall_[n] * cava_fall_[n] * gravity_mod));
      if (cava_out_[n] < 0.0)
        cava_out_[n] = 0.0;
      cava_fall_[n] += bar_fall;
    } else {
      cava_peak_[n] = cava_out_[n];
      cava_fall_[n] = 0.0;
    }
    prev_out_[n] = cava_out_[n];

    // ── Integral smoothing (FPS-normalised, per-bar) ──────────────────────
    cava_out_[n] = cava_mem_[n] * bar_decay + cava_out_[n];
    cava_mem_[n] = cava_out_[n];

    if (cava_out_[n] > 1.0) {
      overshoot = 1;
      cava_out_[n] = 1.0;
    }
    if (cava_out_[n] < 0.0)
      cava_out_[n] = 0.0;
  }

  // ── Auto-sensitivity ──────────────────────────────────────────────────────
  if (auto_sens_enabled_.load()) {
    if (overshoot) {
      sens_ *= (1.0 - 0.02 * fr_mod);
      sens_init_ = false;
    } else if (!silence) {
      sens_ *= (1.0 + 0.001 * fr_mod);
      if (sens_init_)
        sens_ *= (1.0 + 0.04 * fr_mod);
    }
    sens_ = std::max(0.01, std::min(sens_, 1000.0));
  } else {
    sens_ = 1.0;
    sens_init_ = true;
  }

  // ── Monstercat ────────────────────────────────────────────────────────────
  if (mcat_factor_ > 0.0f) {
    std::vector<double> tmp_l(cava_out_.begin(), cava_out_.begin() + num_bars);
    applyMonstercat(tmp_l, (double)mcat_factor_);
    for (int n = 0; n < num_bars; ++n)
      cava_out_[n] = tmp_l[n];

    if (channels_ == 2) {
      std::vector<double> tmp_r(cava_out_.begin() + num_bars,
                                cava_out_.begin() + num_bars * 2);
      applyMonstercat(tmp_r, (double)mcat_factor_);
      for (int n = 0; n < num_bars; ++n)
        cava_out_[n + num_bars] = tmp_r[n];
    }
  }

  // ── Copy to output ────────────────────────────────────────────────────────
  for (int n = 0; n < num_bars; ++n) {
    bars_l_[n] = (float)std::clamp(cava_out_[n], 0.0, 1.0);
    if (channels_ == 2 && !auto_mono_collapsed_)
      bars_r_[n] = (float)std::clamp(cava_out_[n + num_bars], 0.0, 1.0);
    else
      bars_r_[n] = bars_l_[n]; // mono or auto-collapsed stereo
  }
  return true;
}

float FFTProcessor::adjSens(float d) noexcept {
  float s = std::clamp(man_sens_.load() + d, SENS_MIN, SENS_MAX);
  man_sens_.store(s);
  return s;
}
