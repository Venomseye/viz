// Tests for the config/theme watcher and the frame limiter.

#include "config.h"
#include "config_watcher.h"
#include "frame_limiter.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
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

using Clock = std::chrono::steady_clock;
static long msSince(Clock::time_point t) {
  return static_cast<long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t)
          .count());
}
static void settle() {
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
}

int main() {
  // ── theme file name filter ──────────────────────────────────────────────
  CHECK(ConfigWatcher::isThemeFileName("ocean.theme"), "plain theme");
  CHECK(ConfigWatcher::isThemeFileName("a.theme"), "shortest theme");
  CHECK(!ConfigWatcher::isThemeFileName(".theme"), "no base name");
  CHECK(!ConfigWatcher::isThemeFileName("ocean.theme~"), "editor backup");
  CHECK(!ConfigWatcher::isThemeFileName(".ocean.theme.swp"), "vim swap file");
  CHECK(!ConfigWatcher::isThemeFileName(".ocean.theme"), "hidden file");
  CHECK(!ConfigWatcher::isThemeFileName("notes.txt"), "other extension");
  CHECK(!ConfigWatcher::isThemeFileName(nullptr), "null");

#ifdef __linux__
  // ── watcher against real inotify ────────────────────────────────────────
  {
    char tmpl[] = "/tmp/viz_watch_XXXXXX";
    const fs::path root = mkdtemp(tmpl);
    setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
    setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);

    ConfigWatcher w;
    CHECK(w.start(Config::configPath(), (root / "config/viz/themes").string()),
          "watcher starts and creates the directories");
    CHECK(fs::is_directory(root / "config/viz/themes"),
          "themes dir was created");

    auto ev = w.poll();
    CHECK(!ev.config_changed && !ev.themes_changed, "quiet at first");

    // Our own save() must NOT be reported.
    Config c;
    c.theme = 4;
    c.save();
    settle();
    ev = w.poll();
    CHECK(!ev.config_changed, "own save() is not reported as a change");

    // An external edit must be.
    {
      std::ofstream(Config::configPath(), std::ios::app) << "# hand edit\n";
    }
    settle();
    ev = w.poll();
    CHECK(ev.config_changed, "external edit is reported");
    ev = w.poll();
    CHECK(!ev.config_changed, "reported once, not repeatedly");

    // Several writes in one batch -> one report.
    {
      std::ofstream(Config::configPath(), std::ios::app) << "# one\n";
    }
    {
      std::ofstream(Config::configPath(), std::ios::app) << "# two\n";
    }
    settle();
    ev = w.poll();
    CHECK(ev.config_changed, "burst of edits reported");

    // Themes.
    const fs::path th = root / "config/viz/themes";
    {
      std::ofstream(th / "ocean.theme") << "name = O\n";
    }
    settle();
    ev = w.poll();
    CHECK(ev.themes_changed && !ev.config_changed,
          "new .theme reported (and not as config)");

    {
      std::ofstream(th / ".ocean.theme.swp") << "x";
    }
    {
      std::ofstream(th / "ocean.theme~") << "x";
    }
    {
      std::ofstream(th / "readme.txt") << "x";
    }
    settle();
    ev = w.poll();
    CHECK(!ev.themes_changed, "editor temp files and other files are ignored");

    fs::remove(th / "ocean.theme");
    settle();
    ev = w.poll();
    CHECK(ev.themes_changed, "deleting a theme is reported");

    // Config edit and theme edit in the same batch are both delivered.
    {
      std::ofstream(Config::configPath(), std::ios::app) << "# both\n";
    }
    {
      std::ofstream(th / "b.theme") << "name = B\n";
    }
    settle();
    ev = w.poll();
    CHECK(ev.config_changed && ev.themes_changed,
          "a config event does not swallow the rest of the batch");

    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_STATE_HOME");
    fs::remove_all(root);
  }
  {
    ConfigWatcher w;
    CHECK(!w.start("/proc/nonexistent/viz/config", "/proc/nonexistent/themes"),
          "unwatchable location: start() reports failure");
    const auto ev = w.poll();
    CHECK(!ev.config_changed && !ev.themes_changed,
          "a failed watcher polls as quiet");
  }
#endif

  // ── frame limiter ───────────────────────────────────────────────────────
  {
    FrameLimiter lim(50); // 20 ms per frame
    const auto t0 = Clock::now();
    for (int i = 0; i < 25; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(3)); // "work"
      lim.wait();
    }
    const long ms = msSince(t0);
    CHECK(ms >= 470 && ms <= 640,
          "25 frames at 50 fps take ~500 ms (work time does not add)");
  }
  {
    FrameLimiter lim(100); // 10 ms
    lim.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(400)); // long stall
    int instant = 0;
    for (int i = 0; i < 8; ++i) {
      const auto t = Clock::now();
      lim.wait();
      if (msSince(t) < 3)
        ++instant;
    }
    CHECK(instant <= 1, "after a stall frames do not burst to catch up");
  }
  {
    FrameLimiter lim(20);
    CHECK(lim.fps() == 20, "fps()");
    lim.setFps(0);
    CHECK(lim.fps() == 20, "invalid fps ignored");
    lim.setFps(100);
    CHECK(lim.fps() == 100, "fps can change live");
    lim.wait();
    const auto t0 = Clock::now();
    for (int i = 0; i < 20; ++i)
      lim.wait();
    const long ms = msSince(t0);
    CHECK(ms >= 170 && ms <= 300,
          "new rate takes effect (20 frames at 100 fps ~200 ms)");
  }
  std::printf("loop_support: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
