#include "session.h"

#include "audio_utils.h"
#include "user_theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
CaptureWatchdog::Config watchdogConfig(const CliOptions &cli, int fps) {
  CaptureWatchdog::Config c;
  c.pinned = cli.use_mic || !cli.source.empty();
  c.pinned_source =
      cli.use_mic ? std::string(AudioCapture::MIC_SOURCE) : cli.source;
  c.fps = fps;
  return c;
}
} // namespace

Session::Session(Config &cfg, const CliOptions &cli)
    : cfg_(cfg), cli_(cli), channels_(cfg.stereo ? 2 : 1),
      fft_(cli.sample_rate, channels_),
      wd_(watchdogConfig(cli, std::clamp(cfg.fps, 10, 240))) {
  applyFFTConfig(fft_, cfg_);
}

int Session::targetFps() const { return std::clamp(cfg_.fps, 10, 240); }

// ── Audio ────────────────────────────────────────────────────────────────────

void Session::startCapture() {
  doStartAudio(cli_.backend, cli_.source, cli_.use_mic, cli_.sample_rate,
               channels_, fft_, cfg_, audio_, active_source_, bname_);
}

bool Session::startAudio() {
  startCapture();
  wd_.setTrackedMonitor(detectMonitor());
  if (!audio_) {
#if !defined(HAVE_PULSEAUDIO) && !defined(HAVE_PIPEWIRE)
    std::fprintf(stderr,
                 "viz: no audio backend compiled in.\n"
                 "  Install libpulse-dev (PulseAudio) or libpipewire-0.3-dev "
                 "(PipeWire)\n"
                 "  then rebuild: cd build && cmake .. && cmake --build .\n");
#else
    std::fprintf(stderr, "viz: no audio backend started.\n");
#endif
    return false;
  }
  return true;
}

// Stop capture, rebuild the FFT for the current cfg.stereo, restart.  The one
// place that does this (stereo toggle, stereo changed by a config reload).
void Session::restartCapture() {
  if (audio_) {
    audio_->stop();
    audio_.reset();
  }
  channels_ = cfg_.stereo ? 2 : 1;
  fft_.reinit(channels_);
  applyFFTConfig(fft_, cfg_);
  wd_.resetLiveness();
  startCapture();
  wd_.setTrackedMonitor(detectMonitor());
}

void Session::tickWatchdog(long frame) {
  if (!wd_.shouldReconnect(frame, audio_ != nullptr,
                           audio_ && audio_->hasFailed(),
                           [] { return detectMonitor(); }))
    return;

  const bool pinned = wd_.pinned();
  wd_.resetLiveness();
  if (audio_) {
    audio_->stop();
    audio_.reset();
  }
  const std::string mon = pinned ? std::string() : detectMonitor();
  AudioCapture::AudioCallback cb = [this](const float *s, std::size_t n,
                                          int ch) {
    fft_.addSamples(s, n, ch);
  };

  for (const std::string &src : wd_.candidates(mon, active_source_)) {
    audio_ = makeAudio(cli_.backend, src, cli_.sample_rate, channels_, cb);
    if (audio_) {
      bname_ = audio_->backendName();
      // Always record what we actually connected to (including "").
      active_source_ = src;
      if (!pinned && !src.empty()) {
        cfg_.last_source = src;
        cfg_.saveState();
      }
      renderer_.setSourceName(sourceLabel(active_source_));
      break;
    }
  }
  if (audio_)
    wd_.onConnected(mon);
  else
    wd_.onConnectFailed(frame);
}

// ── UI ───────────────────────────────────────────────────────────────────────

void Session::applyRendererConfig() {
  renderer_.setThemeIdx(cfg_.theme); // built-in and user theme indices
  renderer_.setGapWidth(cfg_.gap_width);
  if (!cli_.auto_width)
    renderer_.setBarWidth(cfg_.bar_width);
  renderer_.setHudPinned(cfg_.hud_pinned);
  renderer_.setColourCycle(cfg_.colour_cycle);
  renderer_.setPerBarColour(cfg_.per_bar_colour);
}

bool Session::initUi() {
  renderer_.setColorMode(cli_.color);
  if (!renderer_.init()) {
    if (audio_)
      audio_->stop();
    std::fprintf(stderr,
                 "viz: could not initialise the terminal UI.\n"
                 "  Run it in an interactive terminal (stdout must be a tty) "
                 "and make sure TERM is set (e.g. xterm-256color).\n");
    return false;
  }
  // Load user themes before applying the config so cfg.theme (which may be a
  // user theme index >= Theme::COUNT) validates and clamps correctly.
  renderer_.setUserThemes(loadUserThemes());
  applyRendererConfig();
  if (!renderer_.utf8Ok())
    renderer_.showFeedback("Locale is not UTF-8 - run with LANG=C.UTF-8");
  if (cli_.auto_width)
    renderer_.setBarWidth(renderer_.autoBarWidth());
  renderer_.setSourceName(sourceLabel(active_source_));
  renderer_.notifyChange();
  return true;
}

// ── Reload ───────────────────────────────────────────────────────────────────

void Session::reloadConfig(ReloadSource src) {
  Config nc;
  nc.last_source = cfg_.last_source;
  if (!nc.load())
    return;
  nc.inheritCliOverrides(cfg_); // -t / -f keep winning this session
  const bool stereo_changed = (nc.stereo != cfg_.stereo);
  cfg_ = nc;

  applyRendererConfig();
  applyFFTConfig(fft_, cfg_);
  wd_.setFps(targetFps()); // fps is live-reloadable (main re-reads targetFps())
  if (src == ReloadSource::Signal) {
    // SIGUSR1 also re-reads the themes folder (inotify reports those itself).
    renderer_.setUserThemes(loadUserThemes());
  }
  renderer_.notifyChange();
  if (src == ReloadSource::Signal)
    renderer_.showFeedback("Reloaded");

  if (stereo_changed) {
    restartCapture();
    if (audio_) {
      renderer_.setSourceName(sourceLabel(active_source_));
      if (src == ReloadSource::FileWatch)
        renderer_.showFeedback(cfg_.stereo ? "Stereo" : "Mono");
    }
  }
}

void Session::reloadThemes() {
  renderer_.setUserThemes(loadUserThemes());
  renderer_.showFeedback("Themes reloaded");
}

// ── Keys ─────────────────────────────────────────────────────────────────────

void Session::handleKeys() {
  // Non-blocking.  Pacing is done in ONE place only (FrameLimiter); up to 16
  // queued keys (key-repeat, paste) are handled per frame.
  wtimeout(stdscr, 0);
  for (int keys = 0; keys < 16; ++keys) {
    const int ch = getch();
    if (ch == ERR)
      break;

    switch (ch) {
    case 'q':
      quit_ = true;
      break;

    case 't':
      cfg_.theme = renderer_.nextTheme(); // 0..COUNT-1 + user themes
      cfg_.cli_theme = false;             // the user chose this one: persist it
      cfg_.save();
      break;

    case 'g':
      cfg_.gap_width = renderer_.cycleGap();
      cfg_.save();
      break;

    case ']':
      cfg_.bar_width = renderer_.increaseBarWidth();
      cfg_.save();
      break;

    case '[':
      cfg_.bar_width = renderer_.decreaseBarWidth();
      cfg_.save();
      break;

    case KEY_UP:
      cfg_.sensitivity = fft_.increaseSensitivity();
      cfg_.auto_sens = false;
      fft_.setAutoSens(false);
      renderer_.notifyChange();
      cfg_.save();
      break;

    case KEY_DOWN:
      cfg_.sensitivity = fft_.decreaseSensitivity();
      cfg_.auto_sens = false;
      fft_.setAutoSens(false);
      renderer_.notifyChange();
      cfg_.save();
      break;

    case 'a':
      cfg_.auto_sens = !cfg_.auto_sens;
      fft_.setAutoSens(cfg_.auto_sens);
      renderer_.notifyChange();
      cfg_.save();
      break;

    case 'h':
      renderer_.toggleHudPin();
      cfg_.hud_pinned = renderer_.hudPinned();
      cfg_.save();
      break;

    // ── Stereo / Mono hot-toggle ────────────────────────────────────────
    case 's':
      cfg_.stereo = !cfg_.stereo;
      restartCapture();
      if (audio_) {
        renderer_.setSourceName(sourceLabel(active_source_));
        renderer_.showFeedback(cfg_.stereo ? "Stereo" : "Mono");
        cfg_.save();
      } else {
        // Revert on failure
        cfg_.stereo = !cfg_.stereo;
        restartCapture();
        renderer_.showFeedback("Toggle failed");
      }
      break;

    case 'c':
      renderer_.toggleColourCycle();
      cfg_.colour_cycle = renderer_.colourCycle();
      cfg_.save();
      break;

    case 'v':
      renderer_.togglePerBarColour();
      cfg_.per_bar_colour = renderer_.perBarColour();
      cfg_.save();
      break;

    case 'w':
      cfg_.a_weighting = !cfg_.a_weighting;
      fft_.setAWeighting(cfg_.a_weighting);
      renderer_.showFeedback(cfg_.a_weighting ? "A-Weight On" : "A-Weight Off");
      renderer_.notifyChange();
      cfg_.save();
      break;

    case 'n':
      cfg_.auto_mono = !cfg_.auto_mono;
      fft_.setAutoMono(cfg_.auto_mono);
      renderer_.showFeedback(cfg_.auto_mono ? "Auto-Mono On" : "Auto-Mono Off");
      renderer_.notifyChange();
      cfg_.save();
      break;

    case KEY_RESIZE:
      renderer_.handleResize();
      break;

    default:
      break;
    }
  }
}

// ── Compute + render ─────────────────────────────────────────────────────────

void Session::renderFrame(long frame, double fps) {
  fft_.execute(renderer_.barCount(), static_cast<float>(fps));

  const auto &bl = fft_.barsL();
  float rms = 0.f;
  for (float v : bl)
    rms += v * v;
  rms =
      std::sqrt(rms / static_cast<float>(std::max<std::size_t>(1, bl.size())));
  wd_.onFrameLevel(frame, rms);

  renderer_.render(fft_.barsL(), fft_.barsR(), fps, bname_, fft_.sensitivity(),
                   cfg_.auto_sens);
}

void Session::shutdown() {
  if (audio_) {
    audio_->stop();
    audio_.reset();
  }
  cfg_.save();
  cfg_.saveState();
}
