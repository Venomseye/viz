#include "frame_limiter.h"

#include <cerrno>
#include <ctime>
#include <thread>

int64_t FrameLimiter::nowNs() {
  struct timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

FrameLimiter::FrameLimiter(int fps)
    : fps_(fps > 0 ? fps : 60), budget_ns_(1'000'000'000LL / fps_),
      deadline_ns_(nowNs()) {}

void FrameLimiter::setFps(int fps) {
  if (fps <= 0 || fps == fps_)
    return;
  fps_ = fps;
  budget_ns_ = 1'000'000'000LL / fps_;
}

void FrameLimiter::wait(const std::function<bool()> &keep_sleeping) {
  const int64_t now = nowNs();
  int64_t next = deadline_ns_ + budget_ns_;
  if (now - next > 2 * budget_ns_)
    next = now + budget_ns_; // far behind: resync rather than burst
  deadline_ns_ = next;

#ifdef __linux__
  struct timespec ts{};
  ts.tv_sec = static_cast<time_t>(next / 1'000'000'000LL);
  ts.tv_nsec = static_cast<long>(next % 1'000'000'000LL);
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) ==
             EINTR &&
         (!keep_sleeping || keep_sleeping())) {
  }
#else
  (void)keep_sleeping;
  const int64_t left = next - nowNs();
  if (left > 0)
    std::this_thread::sleep_for(std::chrono::nanoseconds(left));
#endif
}
