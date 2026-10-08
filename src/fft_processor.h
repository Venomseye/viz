#pragma once
/**
 * C++ port of CAVA's cavacore algorithm.
 *  - Dual FFT: bass (2x buffer) + mid/treble (1x buffer)
 *  - Log-distributed cut-off frequencies (CAVA frequency-constant formula)
 *  - Per-bar EQ: (1/2^28) * freq^0.85 / log2(N) / bin_count
 *  - CAVA quadratic-fall gravity model + integral memory
 *  - Auto-sensitivity: overshoot-based feedback
 *  - Monstercat: O(n) two-pass rolling-max, configurable strength
 *  - Rise smoothing: configurable attack coefficient
 *
 *  New in this version:
 *  - A-weighting: IEC 61672 perceptual frequency weighting (optional)
 *  - Noise floor gate: snaps bars below threshold to zero (eliminates idle
 * jitter)
 *  - Per-bar smoothing: bass bars use heavier integral decay + slower fall
 *  - Stereo correlation: auto-collapse to mono when L/R are nearly identical
 *
 * Thread safety:
 *   addSamples() — called from the audio capture thread.
 *   execute()    — called from the main (render) thread.
 *   reinit()     — must only be called when the audio thread is fully stopped.
 */
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fftw3.h>
#include <memory>
#include <vector>

class FFTProcessor {
public:
  static constexpr int LOW_CUT_OFF = 50;          // Hz
  static constexpr int HIGH_CUT_OFF = 20000;      // Hz
  static constexpr int BASS_CUT_OFF = 100;        // Hz — dual-FFT split point
  static constexpr double NOISE_REDUCTION = 0.77; // CAVA integral decay factor

  static constexpr float SENS_MIN = 0.2f;
  static constexpr float SENS_MAX = 8.0f;
  static constexpr float SENS_STEP = 0.1f;
  static constexpr float SENS_DEFAULT = 1.5f;

  static constexpr double MONSTERCAT = 1.5;

  // Stereo auto-collapse: collapse to mono when EMA correlation exceeds ON
  // threshold; restore stereo when it drops below OFF threshold (hysteresis).
  // A frame counts as "silent" for auto-sensitivity when no new sample
  // exceeds this peak (input is scaled to +-32768, so 4 ~= -78 dBFS).
  // Was "exactly 0.0": a single LSB of dither counted as signal, so
  // sensitivity crept up to its cap during pauses and then blasted the
  // display (bars pinned at 1.0 for seconds) when music resumed.
  static constexpr double SILENCE_PEAK = 4.0;

  static constexpr double MONO_CORR_ON = 0.97;
  static constexpr double MONO_CORR_OFF = 0.90;
  static constexpr double CORR_EMA_K = 0.05; // EMA smoothing coefficient

  FFTProcessor(int sample_rate = 44100, int channels = 2);
  ~FFTProcessor();
  FFTProcessor(const FFTProcessor &) = delete;
  FFTProcessor &operator=(const FFTProcessor &) = delete;

  /**
   * Reinitialise in-place for a different channel count.
   * The object's address never changes, so [&] callback captures stay valid.
   * MUST only be called when the audio capture thread is fully stopped.
   */
  void reinit(int new_channels);

  /// Push PCM samples (interleaved) — called from the audio thread.
  /// Lock-free and allocation-free: safe on a real-time audio thread.
  /// Exactly ONE thread may call this at a time (the capture thread; the
  /// owner must stop capture before reinit()).  `samples` is interleaved
  /// with `channels` channels; if that differs from the processor's own
  /// channel count the data is converted (mono -> L=R, N>2 -> first two
  /// channels, stereo -> mono average).
  void addSamples(const float *samples, std::size_t count, int channels);
  void addSamples(const std::vector<float> &samples, int channels) {
    addSamples(samples.data(), samples.size(), channels);
  }

  /// Total samples ever accepted (monotonic).  Usable as a liveness signal:
  /// it advances iff the capture callback is actually delivering data.
  std::uint64_t samplesWritten() const {
    return wcount_.load(std::memory_order_acquire);
  }

  /// Compute one display frame. Returns false only if num_bars <= 0.
  bool execute(int num_bars, float fps);

  /// Bar values in [0,1]. barsR() == barsL() when channels == 1 or auto-mono
  /// active.
  const std::vector<float> &barsL() const { return bars_l_; }
  const std::vector<float> &barsR() const { return bars_r_; }

  // ── Sensitivity ───────────────────────────────────────────────────────────
  float sensitivity() const { return man_sens_.load(); }
  float increaseSensitivity() { return adjSens(+SENS_STEP); }
  float decreaseSensitivity() { return adjSens(-SENS_STEP); }
  void setSensitivity(float v) {
    man_sens_.store(std::clamp(v, SENS_MIN, SENS_MAX));
  }
  void setAutoSens(bool v) { auto_sens_enabled_.store(v); }

  // ── Configurable algorithm parameters ────────────────────────────────────

  /// Fall speed multiplier. 1.0 = CAVA default. Range 0.1–5.0.
  void setGravity(float g) { gravity_factor_ = std::clamp(g, 0.1f, 5.0f); }
  float gravity() const { return gravity_factor_; }

  /// Adjacent-bar propagation. 0 = off, 1.5 = CAVA default, max 5.
  // Any positive value is raised to at least 1.0: the per-step
  // decay is factor*1.5, and a decay <= 1 makes the rolling max GROW along
  // the spectrum (factor 0.6 turns one 0.05 bar into 0.95; 0.3 saturates
  // every bar).  1.0 is the weakest setting that is actually a smoothing.
  void setMonstercat(float m) {
    mcat_factor_ = (m <= 0.0f) ? 0.0f : std::clamp(m, 1.0f, 5.0f);
  }
  float moncatFactor() const { return mcat_factor_; }

  /// Upper frequency limit in Hz. Forces plan rebuild on next execute(). Range
  /// 1000–24000.
  void setHighCutoff(int hz) {
    const int c = std::clamp(hz, 1000, 24000);
    if (c != high_cutoff_) {
      high_cutoff_ = c;
      num_bars_ = 0;
    }
  }
  int highCutoff() const { return high_cutoff_; }

  /// Attack smoothing. 0.0 = instant (original CAVA). Range 0.0–0.95.
  void setRiseFactor(float r) { rise_factor_ = std::clamp(r, 0.0f, 0.95f); }
  float riseFactor() const { return rise_factor_; }

  // ── A-weighting (IEC 61672) ───────────────────────────────────────────────
  /// When enabled, each bar is weighted by perceptual loudness at its centre
  /// frequency. Mids appear relatively louder; sub-bass and extreme highs
  /// are attenuated to match human hearing. Off by default.
  void setAWeighting(bool v) { a_weighting_ = v; }
  bool aWeighting() const { return a_weighting_; }

  // ── Noise floor gate ──────────────────────────────────────────────────────
  /// Bars whose post-sensitivity value is below this threshold are clamped to
  /// zero. Eliminates the idle shimmer when audio is silent or very quiet.
  /// Range 0.0–0.2. Default 0.02.
  void setNoiseGate(float g) { noise_gate_ = std::clamp(g, 0.0f, 0.2f); }
  float noiseGate() const { return noise_gate_; }

  // ── Per-bar smoothing ─────────────────────────────────────────────────────
  /// Extra smoothing applied to bass bars, tapering to zero at treble.
  /// 0.0 = all bars use the same smoothing (original CAVA).
  /// 0.4 = bass bars get noticeably heavier decay and slower fall.
  /// Range 0.0–1.0. Default 0.4.
  void setBassSmooth(float s) { bass_smooth_ = std::clamp(s, 0.0f, 1.0f); }
  float bassSmooth() const { return bass_smooth_; }

  // ── Stereo correlation / auto-mono ────────────────────────────────────────
  /// When enabled and the stereo correlation is above MONO_CORR_ON for a
  /// sustained period, barsR() is set equal to barsL() automatically.
  /// Useful for mono sources that report as stereo (e.g. browser loopback).
  void setAutoMono(bool v) { auto_mono_ = v; }
  bool autoMono() const { return auto_mono_; }

  /// Current auto-sensitivity gain (excludes the manual multiplier).
  double autoGain() const { return sens_; }

  /// Smoothed stereo correlation coefficient [−1, 1]. 1.0 = perfectly mono.
  float stereoCorrelation() const { return (float)corr_ema_; }

  /// True when auto-mono is active AND correlation collapsed the output.
  bool isAutoMonoActive() const { return auto_mono_collapsed_; }

  // ── Info ──────────────────────────────────────────────────────────────────
  int sampleRate() const { return rate_; }
  int channels() const { return channels_; }
  int numBars() const { return num_bars_; }

private:
  int rate_;
  int channels_;
  int num_bars_{0};

  // Buffer sizes — derived from sample rate; never change after construction.
  int fft_buf_size_{0};
  int bass_buf_size_{0};
  int input_buf_size_{0}; // = bass_buf_size_ * channels_; changes on reinit

  // FFTW resources
  double *in_bass_l_{nullptr};
  fftw_complex *out_bass_l_{nullptr};
  double *in_bass_r_{nullptr};
  fftw_complex *out_bass_r_{nullptr};
  double *in_mid_l_{nullptr};
  fftw_complex *out_mid_l_{nullptr};
  double *in_mid_r_{nullptr};
  fftw_complex *out_mid_r_{nullptr};
  fftw_plan plan_bass_l_{nullptr}, plan_bass_r_{nullptr};
  fftw_plan plan_mid_l_{nullptr}, plan_mid_r_{nullptr};

  // Hann windows — built once at construction; depend only on buffer sizes.
  std::vector<double> bass_win_;
  std::vector<double> mid_win_;

  std::vector<double> input_buf_;

  // Per-bar frequency plan
  std::vector<int> cut_lo_;
  std::vector<int> cut_hi_;
  std::vector<double> eq_;
  std::vector<double> aw_; // A-weighting factor per bar [0..num_bars)
  int bass_cut_bar_{0};

  // Per-bar smoothing state
  // Layout: [0..n-1] = left,  [n..2n-1] = right
  std::vector<double> cava_out_;
  std::vector<double> cava_peak_;
  std::vector<double> cava_fall_;
  std::vector<double> cava_mem_;
  std::vector<double> prev_out_;

  std::vector<float> bars_l_, bars_r_;

  // Ring buffer
  // Single-producer / single-consumer ring.  The capture thread stores
  // samples (relaxed atomics: race-free by definition, free on x86/ARM) and
  // publishes the running total with release; execute() reads that total
  // with acquire and copies the newest samples out.  The ring is 4x the
  // largest read, so the producer can never lap the region being read.
  std::unique_ptr<std::atomic<float>[]> ring_;
  std::size_t ring_size_{0};
  std::atomic<std::uint64_t> wcount_{0}; // producer-written
  std::uint64_t rcount_{0};              // consumer-only
  void allocRing();

  double framerate_{60.0};
  int frame_skip_{1};

  // Configurable runtime parameters
  float gravity_factor_{1.0f};
  float mcat_factor_{(float)MONSTERCAT};
  int high_cutoff_{HIGH_CUT_OFF};
  float rise_factor_{0.3f};
  bool a_weighting_{false};
  float noise_gate_{0.02f};
  // Off by default: extra bass smoothing makes the integral accumulate more
  // memory which inflates auto-sens targets and reduces bass reactivity.
  // Users can enable in config (0.1-0.3 is a good range to try).
  float bass_smooth_{0.0f};
  bool auto_mono_{false};

  // Stereo correlation state
  double corr_ema_{1.0};            // EMA of instantaneous correlation
  bool auto_mono_collapsed_{false}; // currently collapsed?

  // Auto-sensitivity state
  double sens_{1.0};
  bool sens_init_{true};
  std::atomic<float> man_sens_{SENS_DEFAULT};
  std::atomic<bool> auto_sens_enabled_{true};

  // Helpers
  void initBufferSizes();
  void buildHannWindows();
  void initFFTW(bool fast);
  void freeFFTW();
  void buildPlan(int num_bars);
  float adjSens(float d) noexcept;
  void applyMonstercat(std::vector<double> &bars, double factor) const;

  static double aWeightFactor(double freq) noexcept;
};
