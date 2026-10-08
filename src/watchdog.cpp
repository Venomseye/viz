#include "watchdog.h"

#include <algorithm>

void CaptureWatchdog::onFrameLevel(long frame, float rms) {
  if (frame <= cfg_.fps) // ignore the first second
    return;
  if (rms < 0.001f) {
    ++silent_frames_;
  } else {
    silent_frames_ = 0;
    heard_audio_ = true;
    silent_retry_secs_ = SILENCE_BASE_SEC; // healthy again: reset the backoff
  }
}

void CaptureWatchdog::resetLiveness() {
  heard_audio_ = false;
  silent_frames_ = 0;
}

void CaptureWatchdog::onConnected(const std::string &monitor) {
  fail_backoff_secs_ = BACKOFF_BASE_SEC;
  next_attempt_frame_ = 0;
  if (!cfg_.pinned)
    tracked_mon_ = monitor;
}

void CaptureWatchdog::onConnectFailed(long frame) {
  // Still down: wait longer each time (2s, 4s, 8s, then every 10s) instead of
  // hammering the sound server.
  next_attempt_frame_ =
      frame + static_cast<long>(cfg_.fps) * fail_backoff_secs_;
  fail_backoff_secs_ = std::min(fail_backoff_secs_ * 2, BACKOFF_MAX_SEC);
}

bool CaptureWatchdog::shouldReconnect(
    long frame, bool have_audio, bool audio_failed,
    const std::function<std::string()> &monitorNow) {
  const long interval = static_cast<long>(cfg_.fps) * 2;
  if (interval <= 0 || frame % interval != 0)
    return false;

  bool reconnect = false;
  if (!have_audio)
    reconnect = (frame >= next_attempt_frame_); // last attempt failed
  else if (audio_failed)
    reconnect = true;

  if (!cfg_.pinned && have_audio && !reconnect) {
    // Default sink changed since we connected.
    if (!tracked_mon_.empty() && monitorNow) {
      const std::string cur = monitorNow();
      if (!cur.empty() && cur != tracked_mon_)
        reconnect = true;
    }
    // Never heard anything on this connection: probably the wrong source.
    // Retry with exponential backoff (5s, 10s, 20s ... 60s).  Once audio has
    // been heard, silence (a paused player) never triggers this.
    if (!reconnect && !heard_audio_ && frame > cfg_.fps &&
        silent_frames_ >= cfg_.fps * silent_retry_secs_) {
      reconnect = true;
      silent_retry_secs_ = std::min(silent_retry_secs_ * 2, SILENCE_MAX_SEC);
    }
  }
  return reconnect;
}

std::vector<std::string>
CaptureWatchdog::candidates(const std::string &monitor,
                            const std::string &active_source) const {
  if (cfg_.pinned)
    return {cfg_.pinned_source};
  return {std::string(), monitor, active_source};
}
