// Scenario tests for the capture watchdog, driven frame by frame.

#include "watchdog.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (cond) {                                                                \
      ++g_pass;                                                                \
    } else {                                                                   \
      ++g_fail;                                                                \
      std::fprintf(stderr, "FAIL  %s:%d  %s\n", __FILE__, __LINE__, msg);      \
    }                                                                          \
  } while (0)

static constexpr int FPS = 60;

// Simulated world the watchdog is driving.
struct Sim {
  CaptureWatchdog wd;
  bool have_audio = true;
  bool audio_failed = false;
  float level = 0.3f; // what the bars look like
  std::string monitor = "a.monitor";
  std::string cur_default = "a.monitor";
  bool connect_ok = true;         // whether a reconnect attempt succeeds
  std::vector<double> reconnects; // seconds at which a reconnect happened
  int monitor_queries = 0;

  explicit Sim(bool pinned = false, const std::string &src = "")
      : wd({pinned, src, FPS}) {
    wd.setTrackedMonitor("a.monitor");
  }

  void run(double from_s, double to_s) {
    for (long f = static_cast<long>(from_s * FPS) + 1;
         f <= static_cast<long>(to_s * FPS); ++f) {
      if (have_audio)
        wd.onFrameLevel(f, level);
      const bool rc = wd.shouldReconnect(f, have_audio, audio_failed, [&]() {
        ++monitor_queries;
        return cur_default;
      });
      if (rc) {
        reconnects.push_back(static_cast<double>(f) / FPS);
        wd.resetLiveness();
        audio_failed = false;
        if (connect_ok) {
          have_audio = true;
          wd.onConnected(cur_default);
        } else {
          have_audio = false;
          wd.onConnectFailed(f);
        }
      }
    }
  }
};

int main() {
  { // A paused player must NEVER cause a reconnect (this was the churn bug).
    Sim s;
    s.run(0, 20); // music
    s.level = 0.0f;
    s.run(20, 20 + 600); // 10 minutes of silence
    CHECK(s.reconnects.empty(), "pause (heard audio before) never reconnects");
  }
  { // Nothing ever heard: retry with exponential backoff, capped at 60 s.
    Sim s;
    s.level = 0.0f;
    s.run(0, 400);
    CHECK(s.reconnects.size() >= 5, "silent-from-start keeps retrying");
    CHECK(s.reconnects[0] >= 5.0 && s.reconnects[0] <= 8.0,
          "first retry after ~5 s");
    std::vector<double> gaps;
    for (size_t i = 1; i < s.reconnects.size(); ++i)
      gaps.push_back(s.reconnects[i] - s.reconnects[i - 1]);
    CHECK(gaps.size() >= 4, "enough gaps to inspect");
    CHECK(gaps[0] >= 9 && gaps[0] <= 12, "gap 1 ~10 s");
    CHECK(gaps[1] >= 19 && gaps[1] <= 22, "gap 2 ~20 s");
    CHECK(gaps[2] >= 39 && gaps[2] <= 42, "gap 3 ~40 s");
    CHECK(gaps[3] >= 59 && gaps[3] <= 62, "gap 4 capped at ~60 s");
    for (double g : gaps)
      CHECK(g <= 62.5, "never waits longer than the cap");
  }
  { // Hearing audio resets the silence backoff.
    Sim s;
    s.level = 0.0f;
    s.run(0, 30); // a couple of retries, backoff grown
    const size_t before = s.reconnects.size();
    CHECK(before >= 2, "retried while silent");
    s.level = 0.3f;
    s.run(30, 40); // audio arrives
    s.level = 0.0f;
    s.run(40, 600); // then a long pause
    CHECK(s.reconnects.size() == before,
          "after audio was heard, silence no longer reconnects");
  }
  { // Stream died: reconnect at the next check.
    Sim s;
    s.run(0, 10);
    s.audio_failed = true;
    s.run(10, 14);
    CHECK(s.reconnects.size() == 1, "failed stream reconnected once");
    CHECK(s.reconnects[0] <= 12.0 + 0.1, "within one 2 s watchdog tick");
  }
  { // Server down: retries 2, 4, 8 s apart then every 10 s; recovers.
    Sim s;
    s.run(0, 4);
    s.connect_ok = false;
    s.audio_failed = true;
    s.run(4, 120);
    CHECK(s.reconnects.size() >= 8, "keeps retrying while down");
    std::vector<double> gaps;
    for (size_t i = 1; i < s.reconnects.size(); ++i)
      gaps.push_back(s.reconnects[i] - s.reconnects[i - 1]);
    CHECK(gaps[0] >= 2 && gaps[0] <= 4.1, "first retry gap ~2 s");
    for (size_t i = 3; i < gaps.size(); ++i)
      CHECK(gaps[i] >= 9.9 && gaps[i] <= 10.1, "steady-state retry every 10 s");
    const size_t n = s.reconnects.size();
    s.connect_ok = true;
    s.run(120, 140);
    CHECK(s.reconnects.size() == n + 1 && s.have_audio,
          "recovers on the first retry after it returns");
    s.run(140, 200);
    CHECK(s.reconnects.size() == n + 1,
          "and then stays connected (backoff reset)");
  }
  { // Default sink changed -> follow it (auto mode only).
    Sim s;
    s.run(0, 10);
    s.cur_default = "b.monitor";
    s.run(10, 14);
    CHECK(s.reconnects.size() == 1,
          "default-sink change triggers one reconnect");
    s.run(14, 60);
    CHECK(s.reconnects.size() == 1,
          "no repeat once tracked monitor is updated");
    s.cur_default = ""; // pactl hiccup returns nothing
    s.run(60, 80);
    CHECK(s.reconnects.size() == 1, "empty monitor answer is ignored");
  }
  { // Pinned (-s / -M): never second-guessed, only recovered.
    Sim s(true, "my.source");
    s.level = 0.0f;
    s.cur_default = "b.monitor";
    s.run(0, 300);
    CHECK(s.reconnects.empty(),
          "pinned: ignores silence and default-sink changes");
    CHECK(s.monitor_queries == 0,
          "pinned: never even asks for the monitor (no subprocess)");
    s.audio_failed = true;
    s.run(300, 304);
    CHECK(s.reconnects.size() == 1, "pinned: still recovers a dead stream");
    s.connect_ok = false;
    s.audio_failed = true;
    s.run(304, 400);
    CHECK(s.reconnects.size() >= 5,
          "pinned: keeps retrying until the device returns");
    CHECK(s.wd.candidates("x", "y") == std::vector<std::string>{"my.source"},
          "pinned: only ever tries the pinned source");
  }
  { // resetLiveness() really forgets what was heard.
    CaptureWatchdog w({false, "", FPS});
    w.onFrameLevel(FPS + 1, 0.5f);
    CHECK(w.heardAudio(), "heard");
    w.resetLiveness();
    CHECK(!w.heardAudio(), "forgotten after resetLiveness()");
  }
  { // Candidate order in auto mode.
    CaptureWatchdog w({false, "", FPS});
    const auto c = w.candidates("m.monitor", "last");
    CHECK(c.size() == 3 && c[0].empty() && c[1] == "m.monitor" &&
              c[2] == "last",
          "auto: default, then detected monitor, then last used");
  }
  { // The first second is ignored (startup), and fps can change live.
    CaptureWatchdog w({false, "", 60});
    w.setTrackedMonitor("a");
    for (long f = 1; f <= 60; ++f)
      w.onFrameLevel(f, 0.5f);
    CHECK(!w.heardAudio(), "first second ignored");
    w.onFrameLevel(61, 0.5f);
    CHECK(w.heardAudio(), "counted afterwards");
    w.setFps(30);
    CHECK(!w.shouldReconnect(59, true, false, nullptr),
          "watch interval follows new fps (60 frames)");
    CHECK(!w.shouldReconnect(60, true, false, nullptr),
          "no reason to reconnect");
    CHECK(w.shouldReconnect(60, true, true, nullptr),
          "failed stream at the new interval");
  }
  std::printf("watchdog: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
