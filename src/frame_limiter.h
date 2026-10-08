#pragma once
#include <chrono>
#include <cstdint>
#include <functional>

/// Paces the main loop to a target frame rate.
///
/// Sleeps until an ABSOLUTE deadline that advances by exactly one frame
/// budget, so per-frame jitter never accumulates and there is no busy-wait.
/// If the loop falls more than two frames behind (stall, suspend/resume, huge
/// resize) it resyncs to "now" instead of bursting to catch up.
class FrameLimiter {
public:
  explicit FrameLimiter(int fps);

  /// Change the target; the current deadline carries over.  No-op if equal.
  void setFps(int fps);
  int fps() const { return fps_; }

  /// Sleep until the next frame is due.  A signal can cut the sleep short; the
  /// sleep is then resumed only while `keep_sleeping` (if given) returns true.
  void wait(const std::function<bool()> &keep_sleeping = nullptr);

private:
  static int64_t nowNs();
  int fps_;
  int64_t budget_ns_;
  int64_t deadline_ns_;
};
