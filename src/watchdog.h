#pragma once
#include <functional>
#include <string>
#include <vector>

/// Decides WHEN the audio capture should be (re)connected.  Pure logic: no
/// audio, no clocks, no I/O, so every scenario is unit-testable.
///
/// Auto mode (default): follow the default sink, retry a source that never
/// produced audio (exponential backoff), fall back through several sources.
/// Pinned mode (-s / -M): the user chose the source; only re-open that same
/// source when the stream dies, retrying until it comes back.
class CaptureWatchdog {
public:
  struct Config {
    bool pinned = false;
    std::string pinned_source; // used for the single candidate when pinned
    int fps = 60;
  };

  explicit CaptureWatchdog(const Config &c) : cfg_(c) {}

  /// Frame rate can change at runtime (live-reloaded config).
  void setFps(int fps) { cfg_.fps = fps; }

  // ── Inputs ────────────────────────────────────────────────────────────
  /// Once per frame, with the RMS of the left bars.  Frames in the first
  /// second are ignored (startup).
  void onFrameLevel(long frame, float rms);

  /// Forget "heard audio / silent frames" (new connection, stereo toggle...).
  void resetLiveness();

  /// Remember the default monitor seen when we (re)connected.
  void setTrackedMonitor(const std::string &m) { tracked_mon_ = m; }

  /// A (re)connect attempt succeeded / failed.  `monitor` is the default
  /// monitor seen at that time (ignored in pinned mode).
  void onConnected(const std::string &monitor);
  void onConnectFailed(long frame);

  // ── Decision ──────────────────────────────────────────────────────────
  /// True when the capture should be reconnected now.  Only evaluates every
  /// 2 seconds' worth of frames.  `monitorNow` is called lazily (it may run a
  /// subprocess) and only in auto mode.
  bool shouldReconnect(long frame, bool have_audio, bool audio_failed,
                       const std::function<std::string()> &monitorNow);

  /// Sources to try, in order.
  std::vector<std::string> candidates(const std::string &monitor,
                                      const std::string &active_source) const;

  // ── Introspection (tests / diagnostics) ───────────────────────────────
  bool heardAudio() const { return heard_audio_; }
  bool pinned() const { return cfg_.pinned; }

  static constexpr int SILENCE_BASE_SEC = 5;
  static constexpr int SILENCE_MAX_SEC = 60;
  static constexpr int BACKOFF_BASE_SEC = 2;
  static constexpr int BACKOFF_MAX_SEC = 10;

private:
  Config cfg_;
  bool heard_audio_ = false;
  int silent_frames_ = 0;
  int silent_retry_secs_ = SILENCE_BASE_SEC;
  long next_attempt_frame_ = 0;
  int fail_backoff_secs_ = BACKOFF_BASE_SEC;
  std::string tracked_mon_;
};
