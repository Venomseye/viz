#pragma once
#include "audio_capture.h"
#include "cli_options.h"
#include "config.h"
#include "fft_processor.h"
#include "renderer.h"
#include "watchdog.h"

#include <memory>
#include <string>

/// Everything that changes while viz runs: the audio capture, the FFT stage,
/// the renderer and the config they are driven by.  main() owns the process
/// concerns (signals, the loop, pacing); Session owns the behaviour.
///
/// Not movable: the audio callback holds a pointer to `fft_`, so its address
/// must stay stable for the Session's lifetime.
class Session {
public:
  Session(Config &cfg, const CliOptions &cli);
  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;

  /// Start capture.  Prints the reason and returns false if no backend works.
  bool startAudio();
  /// Bring up ncurses and apply the config.  Prints the reason and returns
  /// false on failure (capture is stopped).
  bool initUi();

  // ── Per frame ─────────────────────────────────────────────────────────
  void handleKeys();
  void tickWatchdog(long frame);
  void renderFrame(long frame, double fps);

  // ── Events ────────────────────────────────────────────────────────────
  enum class ReloadSource { Signal, FileWatch };
  void reloadConfig(ReloadSource src);
  void reloadThemes();
  void handleResize() { renderer_.handleResize(); }

  /// Stop capture and persist config/state.
  void shutdown();

  bool quitRequested() const { return quit_; }
  /// Target frame rate from the CURRENT config, clamped to 10..240.
  int targetFps() const;

private:
  void startCapture(); // thin wrapper over doStartAudio()
  void restartCapture();
  void applyRendererConfig();

  Config &cfg_;
  CliOptions cli_;
  int channels_;
  FFTProcessor fft_;
  Renderer renderer_;
  std::unique_ptr<AudioCapture> audio_;
  std::string active_source_;
  std::string bname_;
  CaptureWatchdog wd_;
  bool quit_ = false;
};
