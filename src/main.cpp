#include "audio_capture.h"
#include "audio_utils.h"
#include "cli_options.h"
#include "config.h"
#include "config_watcher.h"
#include "frame_limiter.h"
#include "session.h"
#include "terminal_caps.h"
#include "text_utils.h"
#include "user_theme.h"

// Defined by CMakeLists.txt via target_compile_definitions; fall back to
// "unknown" when the binary is built without CMake (e.g. a plain Makefile).
#ifndef VIZ_VERSION
#define VIZ_VERSION "unknown"
#endif

#include <atomic>
#include <chrono>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <signal.h>
#include <string>

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_resize{false};
static std::atomic<bool> g_reload{false}; // set by SIGUSR1 for live reload
static void sig_handler(int s) {
  // SIGHUP is what the kernel sends when the controlling terminal goes away
  // (window closed, SSH dropped).  It must terminate us, never reload.
  if (s == SIGINT || s == SIGTERM || s == SIGHUP)
    g_running.store(false);
  else if (s == SIGWINCH)
    g_resize.store(true);
  else if (s == SIGUSR1)
    g_reload.store(true);
}

// ── --check ──────────────────────────────────────────────────────────────────
static int doCheck(const Config &cfg, const CliOptions &cli) {
  const auto themes = loadUserThemes();
  const std::string mon = detectMonitor();
  bool ok = true;

  printf("viz %s — config check\n", VIZ_VERSION);
  printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n");

  printf("Paths:\n");
  printf("  config  = %s\n", Config::configPath().c_str());
  printf("  state   = %s\n", Config::statePath().c_str());
  printf("  themes  = %s  (%zu loaded)\n\n", themesDir().c_str(),
         themes.size());

  printf("Visual:\n");
  const int total_themes =
      static_cast<int>(Theme::COUNT) + static_cast<int>(themes.size());
  if (cfg.theme < 0 || cfg.theme >= total_themes) {
    printf("  theme       = %d  ✗ out of range (0-%d)\n", cfg.theme,
           total_themes - 1);
    ok = false;
  } else if (cfg.theme < static_cast<int>(Theme::COUNT)) {
    printf("  theme       = %d  (%s)\n", cfg.theme,
           builtinThemeName(cfg.theme));
  } else {
    const int ui = cfg.theme - static_cast<int>(Theme::COUNT);
    printf("  theme       = %d  (user: %s)\n", cfg.theme,
           themes[static_cast<std::size_t>(ui)].name.c_str());
  }
  printf("  bar_width   = %d\n", cfg.bar_width);
  printf("  gap_width   = %d\n", cfg.gap_width);
  printf("  hud_pinned  = %s\n\n", cfg.hud_pinned ? "yes" : "no");

  printf("FFT / Smoothing:\n");
  printf("  gravity     = %.2f\n", static_cast<double>(cfg.gravity));
  printf("  monstercat  = %.2f\n", static_cast<double>(cfg.monstercat));
  printf("  rise_factor = %.2f\n", static_cast<double>(cfg.rise_factor));
  printf("  bass_smooth = %.2f\n", static_cast<double>(cfg.bass_smooth));
  printf("  noise_gate  = %.3f\n", static_cast<double>(cfg.noise_gate));
  printf("  sensitivity = %.2f   auto=%s\n\n",
         static_cast<double>(cfg.sensitivity), cfg.auto_sens ? "yes" : "no");

  printf("Audio:\n");
  printf("  stereo      = %s\n", cfg.stereo ? "yes" : "no");
  printf("  high_cutoff = %d Hz\n", cfg.high_cutoff);
  printf("  a_weighting = %s\n", cfg.a_weighting ? "yes" : "no");
  printf("  auto_mono   = %s\n", cfg.auto_mono ? "yes" : "no");
  if (mon.empty()) {
    printf("  monitor     = (not found — is PipeWire/PulseAudio running?)\n");
    ok = false;
  } else {
    printf("  monitor     = %s\n", mon.c_str());
  }
  if (!cfg.last_source.empty())
    printf("  last_source = %s\n", cfg.last_source.c_str());
  printf("\n");

  if (!themes.empty()) {
    printf("User themes:\n");
    for (const auto &t : themes)
      printf("  %s\n", t.name.c_str());
    printf("\n");
  }

  printf("Terminal:\n");
  {
    const char *term = std::getenv("TERM");
    const char *ct = std::getenv("COLORTERM");
    printf("  TERM        = %s\n", term ? term : "(unset)");
    printf("  COLORTERM   = %s\n", ct ? ct : "(unset)");
    printf("  truecolor   = %s\n",
           detectTruecolor() ? "detected" : "not detected");
    printf("  xterm-direct terminfo = %s\n",
           terminfoEntryExists("xterm-direct") ? "found" : "missing");
    const std::string ov = termOverride(cli.color);
    printf("  --color     = %s  ->  TERM override: %s\n",
           colorModeName(cli.color), ov.empty() ? "none" : ov.c_str());
    // Mirror what Renderer::init() will do: fall back to C.UTF-8 if needed.
    std::setlocale(LC_ALL, "");
    const bool env_utf8 = localeIsUtf8();
    const bool utf8 = env_utf8 || initUtf8Locale();
    printf("  locale      = %s\n\n",
           env_utf8 ? "UTF-8"
           : utf8   ? "not UTF-8 in your environment; viz switches to C.UTF-8"
                    : "not UTF-8 and no UTF-8 locale installed (bars may "
                      "render wrongly)");
  }

  printf("Performance:\n");
  printf("  fps         = %d\n\n", cfg.fps);

  printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
  printf("%s\n",
         ok ? "All checks passed." : "One or more checks failed — see above.");
  return ok ? 0 : 1;
}

// ── Apply rendering config
// ────────────────────────────────────────────────────

// Safe integer CLI argument parser — prints a helpful error and exits cleanly
// instead of letting std::stoi throw an unhandled exception.

// ── main ─────────────────────────────────────────────────────────────────────
int main(int argc, char *argv[]) {
  struct sigaction sa{};
  sa.sa_handler = sig_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGWINCH, &sa, nullptr);
  sigaction(SIGHUP, &sa, nullptr);  // terminal closed -> clean shutdown
  sigaction(SIGUSR1, &sa, nullptr); // live reload: pkill -USR1 -x viz

  // Settings used to live in .../cava-viz/ (the project's former name).
  if (Config::migrateLegacyDirs() > 0)
    std::fprintf(stderr,
                 "viz: moved your settings from \"cava-viz\" to \"viz\"\n");

  const CliResult parsed = parseCli(argc, argv);
  if (!parsed.ok) {
    if (!parsed.error.empty())
      std::fprintf(stderr, "%s\n", parsed.error.c_str());
    if (parsed.show_usage)
      printUsage(argv[0]);
    return 1;
  }
  const CliOptions &cli = parsed.opts;

  Config cfg;
  cfg.load();
  cfg.loadState();

  // -t / -f apply to this run only and are never written back to the file.
  if (cli.theme >= 0) {
    cfg.theme = cli.theme;
    cfg.cli_theme = true;
  }
  if (cli.fps >= 0) {
    cfg.fps = cli.fps;
    cfg.cli_fps = true;
  }

  switch (cli.action) {
  case CliAction::Help:
    printUsage(argv[0]);
    return 0;
  case CliAction::Version:
    std::printf("viz %s\n", VIZ_VERSION);
    return 0;
  case CliAction::ListSources:
    printSources(cfg);
    return 0;
  case CliAction::Check:
    return doCheck(cfg, cli);
  case CliAction::Run:
    break;
  }

  Session session(cfg, cli);
  if (!session.startAudio() || !session.initUi())
    return 1;

  ConfigWatcher watcher;
  watcher.start(Config::configPath(), themesDir());

  FrameLimiter limiter(session.targetFps());
  using Clock = std::chrono::steady_clock;
  double fps = static_cast<double>(session.targetFps());
  int fcount = 0;
  long frames = 0;
  auto fps_tp = Clock::now();

  // ── Main loop ─────────────────────────────────────────────────────────────
  while (g_running.load() && !session.quitRequested()) {
    ++frames;
    if (g_resize.exchange(false))
      session.handleResize();

    // Live reload via `pkill -USR1 -x viz` (works over SSH / without inotify).
    if (g_reload.exchange(false))
      session.reloadConfig(Session::ReloadSource::Signal);

    // Live reload via inotify: config edits and theme files.
    const ConfigWatcher::Events ev = watcher.poll();
    if (ev.config_changed)
      session.reloadConfig(Session::ReloadSource::FileWatch);
    if (ev.themes_changed)
      session.reloadThemes();

    session.tickWatchdog(frames);
    session.handleKeys();
    session.renderFrame(frames, fps);

    // ── FPS tracking ────────────────────────────────────────────────────────
    ++fcount;
    if (frames > session.targetFps()) {
      const auto now = Clock::now();
      const double el = std::chrono::duration<double>(now - fps_tp).count();
      if (el >= 1.0) {
        fps = fcount / el;
        fcount = 0;
        fps_tp = now;
      }
    }
    // ── Frame limiter ───────────────────────────────────────────────────────
    limiter.setFps(session.targetFps()); // fps may have changed on reload
    limiter.wait([] {
      return g_running.load() && !g_resize.load() && !g_reload.load();
    });
  }

  session.shutdown();
  return 0;
}
