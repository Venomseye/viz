// tests/test_config.cpp
// Config round-trip and parser correctness tests.
// Compile: g++ -std=c++17 -Isrc -o test_config tests/test_config.cpp
// src/config.cpp Run:     ./test_config

#include "config.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>

namespace fs = std::filesystem;

// ── Helpers
// ───────────────────────────────────────────────────────────────────

static int tests_run = 0;
static int tests_failed = 0;

static void check_bool(const char *label, bool got, bool expected) {
  ++tests_run;
  if (got == expected) {
    printf("  \033[32mPASS\033[0m  %s\n", label);
  } else {
    ++tests_failed;
    printf("  \033[31mFAIL\033[0m  %s  expected=%s got=%s\n", label,
           expected ? "true" : "false", got ? "true" : "false");
  }
}

static void check_int(const char *label, int got, int expected) {
  ++tests_run;
  if (got == expected) {
    printf("  \033[32mPASS\033[0m  %s\n", label);
  } else {
    ++tests_failed;
    printf("  \033[31mFAIL\033[0m  %s  expected=%d got=%d\n", label, expected,
           got);
  }
}

static void check_float(const char *label, float got, float expected,
                        float tol = 0.001f) {
  ++tests_run;
  const float diff = got - expected;
  const float abs_diff = diff < 0 ? -diff : diff;
  if (abs_diff <= tol) {
    printf("  \033[32mPASS\033[0m  %s\n", label);
  } else {
    ++tests_failed;
    printf("  \033[31mFAIL\033[0m  %s  expected=%.4f got=%.4f\n", label,
           static_cast<double>(expected), static_cast<double>(got));
  }
}

static void check_str(const char *label, const std::string &got,
                      const std::string &expected) {
  ++tests_run;
  if (got == expected) {
    printf("  \033[32mPASS\033[0m  %s\n", label);
  } else {
    ++tests_failed;
    printf("  \033[31mFAIL\033[0m  %s  expected='%s' got='%s'\n", label,
           expected.c_str(), got.c_str());
  }
}

// RAII temp directory — creates a unique dir under /tmp and sets XDG env vars
// so Config::configPath() / statePath() point into it for the duration of the
// test.
struct TempDir {
  fs::path path;

  TempDir() {
    char tmpl[] = "/tmp/viz_test_XXXXXX";
    const char *d = mkdtemp(tmpl);
    if (!d) {
      perror("mkdtemp");
      std::exit(1);
    }
    path = d;
    setenv("XDG_CONFIG_HOME", (path / "config").c_str(), 1);
    setenv("XDG_STATE_HOME", (path / "state").c_str(), 1);
  }
  ~TempDir() {
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_STATE_HOME");
    fs::remove_all(path);
  }
};

// Write a config file directly (bypassing Config::save) for malformed-input
// tests.
static void writeRawConfig(const std::string &content) {
  const fs::path p = Config::configPath();
  fs::create_directories(p.parent_path());
  std::ofstream f(p);
  f << content;
}

// ── Test groups
// ───────────────────────────────────────────────────────────────

static void testRoundTrip() {
  printf("\n[round-trip: save then load]\n");
  TempDir td;

  Config orig;
  orig.theme = 5;
  orig.bar_width = 4;
  orig.gap_width = 2;
  orig.hud_pinned = true;
  orig.colour_cycle = true;
  orig.per_bar_colour = true;
  orig.stereo = false;
  orig.high_cutoff = 12000;
  orig.gravity = 2.50f;
  orig.monstercat = 0.75f;
  orig.rise_factor = 0.85f;
  orig.bass_smooth = 0.20f;
  orig.a_weighting = true;
  orig.noise_gate = 0.015f;
  orig.auto_mono = true;
  orig.sensitivity = 3.14f;
  orig.auto_sens = false;
  orig.fps = 30;

  orig.save();

  Config loaded;
  const bool ok = loaded.load();
  check_bool("load() returns true", ok, true);
  check_int("theme", loaded.theme, orig.theme);
  check_int("bar_width", loaded.bar_width, orig.bar_width);
  check_int("gap_width", loaded.gap_width, orig.gap_width);
  check_bool("hud_pinned", loaded.hud_pinned, orig.hud_pinned);
  check_bool("colour_cycle", loaded.colour_cycle, orig.colour_cycle);
  check_bool("per_bar_colour", loaded.per_bar_colour, orig.per_bar_colour);
  check_bool("stereo", loaded.stereo, orig.stereo);
  check_int("high_cutoff", loaded.high_cutoff, orig.high_cutoff);
  check_float("gravity", loaded.gravity, orig.gravity);
  check_float("monstercat", loaded.monstercat, orig.monstercat);
  check_float("rise_factor", loaded.rise_factor, orig.rise_factor);
  check_float("bass_smooth", loaded.bass_smooth, orig.bass_smooth);
  check_bool("a_weighting", loaded.a_weighting, orig.a_weighting);
  check_float("noise_gate", loaded.noise_gate, orig.noise_gate);
  check_bool("auto_mono", loaded.auto_mono, orig.auto_mono);
  check_float("sensitivity", loaded.sensitivity, orig.sensitivity);
  check_bool("auto_sens", loaded.auto_sens, orig.auto_sens);
  check_int("fps", loaded.fps, orig.fps);
}

static void testCommentPreservation() {
  printf("\n[comment preservation: save preserves user comments]\n");
  TempDir td;

  // Write a config with a user comment and a custom blank line layout.
  writeRawConfig("# My custom comment\n"
                 "theme = 3\n"
                 "\n"
                 "# Another comment\n"
                 "fps = 45\n");

  Config c;
  c.load();
  c.bar_width = 6; // change one field
  c.save();

  // Read the raw file and check comments survived.
  std::ifstream f(Config::configPath());
  std::string contents((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());

  ++tests_run;
  const bool has_comment =
      contents.find("# My custom comment") != std::string::npos;
  if (has_comment)
    printf("  \033[32mPASS\033[0m  user comment preserved\n");
  else {
    ++tests_failed;
    printf("  \033[31mFAIL\033[0m  user comment was lost\n");
  }

  ++tests_run;
  const bool has_another =
      contents.find("# Another comment") != std::string::npos;
  if (has_another)
    printf("  \033[32mPASS\033[0m  second comment preserved\n");
  else {
    ++tests_failed;
    printf("  \033[31mFAIL\033[0m  second comment was lost\n");
  }

  // Verify the changed value was actually written.
  Config reload;
  reload.load();
  check_int("modified bar_width saved correctly", reload.bar_width, 6);
  check_int("existing theme value preserved", reload.theme, 3);
  check_int("existing fps value preserved", reload.fps, 45);
}

static void testDefaults() {
  printf("\n[defaults: missing file returns defaults]\n");
  TempDir td;

  // Do not write any config — XDG dir is empty.
  Config c;
  const bool loaded = c.load();
  // load() should return false (file absent) but leave fields at defaults.
  check_bool("load() returns false for missing file", loaded, false);
  check_int("default theme", c.theme, 0);
  check_int("default fps", c.fps, 60);
  check_float("default gravity", c.gravity, 1.0f);
  check_bool("default auto_sens", c.auto_sens, true);
  check_bool("default stereo", c.stereo, true);
}

static void testClampValidation() {
  printf("\n[clamping: out-of-range values are clamped]\n");
  TempDir td;

  writeRawConfig("bar_width  = 999\n"  // above max 8
                 "gap_width  = -5\n"   // below min 0
                 "fps        = 9999\n" // above max 240
                 "gravity    = 99.0\n" // above max 5.0
                 "noise_gate = 0.99\n" // above max 0.2
                 "sensitivity = 0.0\n" // below min 0.2
  );
  Config c;
  c.load();
  check_int("bar_width clamped to 8", c.bar_width, 8);
  check_int("gap_width clamped to 0", c.gap_width, 0);
  check_int("fps clamped to 240", c.fps, 240);
  check_float("gravity clamped to 5.0", c.gravity, 5.0f);
  check_float("noise_gate clamped to 0.2", c.noise_gate, 0.2f);
  check_float("sensitivity clamped to 0.2", c.sensitivity, 0.2f);
}

static void testMalformedFloat() {
  printf("\n[malformed floats: bad values keep existing defaults]\n");
  TempDir td;

  writeRawConfig("gravity = banana\n"
                 "fps = 60\n");
  Config c;
  c.load();
  // gravity should remain at its struct default (1.0f) because "banana" is
  // invalid.
  check_float("malformed gravity keeps default", c.gravity, 1.0f);
  check_int("valid fps still parsed", c.fps, 60);
}

static void testStateRoundTrip() {
  printf("\n[state round-trip: last_source saved and restored]\n");
  TempDir td;

  Config c;
  c.last_source = "alsa_output.pci-0000_00_1f.3.monitor";
  c.saveState();

  Config c2;
  c2.loadState();
  check_str("last_source restored", c2.last_source,
            "alsa_output.pci-0000_00_1f.3.monitor");
}

static void testThemeIndexNotClamped() {
  printf("\n[theme index: values above 11 not clamped (user themes)]\n");
  TempDir td;

  writeRawConfig("theme = 15\n");
  Config c;
  c.load();
  // theme >= 0 should pass through; the renderer enforces the real upper bound.
  check_int("theme=15 not clamped by config", c.theme, 15);
}

// ── main ─────────────────────────────────────────────────────────────────────

// ── Atomic save, no-op skip, symlink + mode preservation, self-write digest ──
static std::string slurp(const fs::path &p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}
static int countTmpFiles(const fs::path &dir) {
  int n = 0;
  for (const auto &e : fs::directory_iterator(dir))
    if (e.path().filename().string().find(".tmp.") != std::string::npos)
      ++n;
  return n;
}

static void testAtomicSave() {
  printf("\n[atomic save / self-write detection]\n");
  TempDir td;
  const fs::path cfgp = Config::configPath();

  Config c;
  c.theme = 3;
  c.save(); // first-time template path
  check_bool("first save creates the file", fs::exists(cfgp), true);
  check_int("no temp files left after first save",
            countTmpFiles(cfgp.parent_path()), 0);
  check_bool("digest of last save == digest of file on disk",
             Config::lastSavedDigest() == Config::currentFileDigest() &&
                 Config::currentFileDigest() != 0,
             true);

  // Unchanged save must not touch the file at all (same inode, same bytes).
  struct stat before{};
  stat(cfgp.c_str(), &before);
  const std::string text_before = slurp(cfgp);
  c.save();
  struct stat after{};
  stat(cfgp.c_str(), &after);
  check_bool("unchanged save does not rewrite the file (same inode)",
             before.st_ino == after.st_ino, true);
  check_bool("unchanged save leaves content identical",
             slurp(cfgp) == text_before, true);

  // Hand-formatted lines and inline comments survive saves that don't change
  // that value; only the changed line is rewritten.
  {
    std::ofstream(cfgp) << "theme        =   3    # my favourite\n"
                           "gravity      = 1.0    # slow fall\n";
    Config k;
    k.load();
    k.save(); // nothing changed
    check_bool("unchanged values keep spacing and inline comments",
               slurp(cfgp).find("theme        =   3    # my favourite") !=
                       std::string::npos &&
                   slurp(cfgp).find("gravity      = 1.0    # slow fall") !=
                       std::string::npos,
               true);
    k.theme = 5;
    k.save();
    const std::string t = slurp(cfgp);
    check_bool("changed line is rewritten",
               t.find("theme = 5") != std::string::npos, true);
    check_bool("untouched neighbour keeps its inline comment",
               t.find("gravity      = 1.0    # slow fall") != std::string::npos,
               true);
    Config k2;
    k2.theme = 3;
    k2.save(); // restore a template-style file for the rest of the test
    fs::remove(cfgp);
    Config fresh;
    fresh.theme = 3;
    fresh.save();
  }

  // A real change is written, atomically, and is recognised as our own.
  c.theme = 7;
  c.save();
  Config r;
  r.load();
  check_int("changed value is persisted", r.theme, 7);
  check_int("no temp files left after update",
            countTmpFiles(cfgp.parent_path()), 0);
  check_bool("digest matches after update",
             Config::lastSavedDigest() == Config::currentFileDigest(), true);

  // An external edit must NOT look like our own write.
  {
    std::ofstream out(cfgp, std::ios::app);
    out << "# edited by hand\n";
  }
  check_bool("external edit changes digest (so it gets reloaded)",
             Config::lastSavedDigest() != Config::currentFileDigest(), true);

  // Permission bits survive a save (config may hold a private source name).
  chmod(cfgp.c_str(), 0600);
  c.theme = 2;
  c.save();
  struct stat pm{};
  stat(cfgp.c_str(), &pm);
  check_int("file mode preserved across save",
            static_cast<int>(pm.st_mode & 0777), 0600);
}

static void testSymlinkedConfig() {
  printf("\n[symlinked config (dotfile managers)]\n");
  TempDir td;
  const fs::path cfgp = Config::configPath();
  fs::create_directories(cfgp.parent_path());
  const fs::path real_dir = td.path / "dotfiles";
  fs::create_directories(real_dir);
  const fs::path real = real_dir / "viz.conf";
  {
    std::ofstream(real) << "theme = 4\n# my note\n";
  }
  fs::create_symlink(real, cfgp);

  Config c;
  c.load();
  check_int("loads through the symlink", c.theme, 4);
  c.theme = 9;
  c.save();

  check_bool("config path is still a symlink after save", fs::is_symlink(cfgp),
             true);
  check_bool("symlink still points at the original target",
             fs::read_symlink(cfgp) == real, true);
  check_bool("target file got the new value",
             slurp(real).find("theme = 9") != std::string::npos, true);
  check_bool("comments in the target preserved",
             slurp(real).find("# my note") != std::string::npos, true);
  check_int("no temp files in dotfiles dir", countTmpFiles(real_dir), 0);
}

static void testCliOverridesNotPersisted() {
  printf("\n[CLI overrides (-t / -f) are session-only]\n");
  TempDir td;
  const fs::path cfgp = Config::configPath();
  fs::create_directories(cfgp.parent_path());
  {
    std::ofstream(cfgp) << "theme = 2\nfps = 45\ngravity = 1.00\n";
  }

  Config c;
  c.load();
  c.theme = 6;
  c.cli_theme = true; // as if `viz -t 6 -f 30`
  c.fps = 30;
  c.cli_fps = true;
  c.gravity = 2.5f; // an unrelated, genuinely persisted change
  c.save();

  Config r;
  r.load();
  check_int("overridden theme not written (file keeps 2)", r.theme, 2);
  check_int("overridden fps not written (file keeps 45)", r.fps, 45);
  check_float("unrelated change IS written", r.gravity, 2.5f);

  // An interactive choice clears the flag and is persisted.
  c.theme = 4;
  c.cli_theme = false;
  c.save();
  Config r2;
  r2.load();
  check_int("interactive theme change is persisted", r2.theme, 4);
  check_int("fps override still not persisted", r2.fps, 45);

  // A reload keeps the command line winning over the file.
  Config fresh;
  fresh.load(); // theme 4 / fps 45 from disk
  fresh.inheritCliOverrides(c);
  check_int("reload keeps CLI fps", fresh.fps, 30);
  check_int("reload takes theme from file once it is no longer an override",
            fresh.theme, 4);
  Config withTheme;
  withTheme.theme = 9;
  withTheme.cli_theme = true;
  fresh.inheritCliOverrides(withTheme);
  check_int("reload re-applies a CLI theme", fresh.theme, 9);

  // Creating a config from scratch must not bake the override in either.
  fs::remove(cfgp);
  Config n;
  n.theme = 7;
  n.cli_theme = true;
  n.save();
  Config r3;
  r3.load();
  check_bool("fresh file gets the default theme, not the CLI one",
             r3.theme != 7, true);
}

// ── Migration from the pre-rename "cava-viz" directories ─────────────────────
static void testLegacyMigration() {
  printf("\n[migration: cava-viz -> viz]\n");

  // New paths use "viz".
  {
    TempDir td;
    check_bool("config path ends in /viz/config",
               Config::configPath().find("/viz/config") != std::string::npos &&
                   Config::configPath().find("cava-viz") == std::string::npos,
               true);
    check_bool("state path ends in /viz/state",
               Config::statePath().find("/viz/state") != std::string::npos &&
                   Config::statePath().find("cava-viz") == std::string::npos,
               true);
  }

  // Old settings, themes and state are carried over intact.
  {
    TempDir td;
    const fs::path cfg_old = td.path / "config" / "cava-viz";
    const fs::path st_old = td.path / "state" / "cava-viz";
    fs::create_directories(cfg_old / "themes");
    fs::create_directories(st_old);
    {
      std::ofstream(cfg_old / "config") << "theme = 5\n# keep me\n";
    }
    {
      std::ofstream(cfg_old / "themes" / "mine.theme") << "name = Mine\n";
    }
    {
      std::ofstream(st_old / "state") << "last_source = my.monitor\n";
    }

    check_int("two directories migrated", Config::migrateLegacyDirs(), 2);
    check_bool("old config dir is gone", !fs::exists(cfg_old), true);
    check_bool("old state dir is gone", !fs::exists(st_old), true);
    check_bool("user theme file moved with it",
               fs::exists(td.path / "config" / "viz" / "themes" / "mine.theme"),
               true);

    Config c;
    check_bool("config loads from the new location", c.load(), true);
    check_int("migrated setting is preserved", c.theme, 5);
    c.loadState();
    check_bool("state loads from the new location",
               c.last_source == "my.monitor", true);
    check_bool("comments in migrated config preserved",
               slurp(Config::configPath()).find("# keep me") !=
                   std::string::npos,
               true);

    check_int("second call is a no-op", Config::migrateLegacyDirs(), 0);
  }

  // If viz/ already exists it always wins and cava-viz/ is left untouched.
  {
    TempDir td;
    const fs::path cfg_old = td.path / "config" / "cava-viz";
    const fs::path cfg_new = td.path / "config" / "viz";
    fs::create_directories(cfg_old);
    fs::create_directories(cfg_new);
    {
      std::ofstream(cfg_old / "config") << "theme = 9\n";
    }
    {
      std::ofstream(cfg_new / "config") << "theme = 2\n";
    }

    check_int("nothing moved when viz/ exists", Config::migrateLegacyDirs(), 0);
    Config c;
    c.load();
    check_int("new location wins", c.theme, 2);
    check_bool("legacy dir left untouched", fs::exists(cfg_old / "config"),
               true);
  }

  // Nothing to migrate: no-op and no directories invented.
  {
    TempDir td;
    check_int("fresh install: nothing to do", Config::migrateLegacyDirs(), 0);
    check_bool("no viz dir created by migration",
               !fs::exists(td.path / "config" / "viz"), true);
  }

  // A symlinked legacy dir (dotfile manager) moves as a symlink: the target
  // and its contents are not copied or deleted.
  {
    TempDir td;
    const fs::path real = td.path / "dotfiles" / "viz-config";
    fs::create_directories(real);
    {
      std::ofstream(real / "config") << "theme = 7\n";
    }
    fs::create_directories(td.path / "config");
    fs::create_directory_symlink(real, td.path / "config" / "cava-viz");

    check_int("symlinked dir migrated", Config::migrateLegacyDirs(), 1);
    check_bool("it is still a symlink at the new name",
               fs::is_symlink(td.path / "config" / "viz"), true);
    check_bool("dotfiles target untouched", fs::exists(real / "config"), true);
    Config c;
    c.load();
    check_int("config readable through the symlink", c.theme, 7);
  }

  // A dangling legacy symlink is ignored rather than moved.
  {
    TempDir td;
    fs::create_directories(td.path / "config");
    fs::create_directory_symlink(td.path / "nowhere",
                                 td.path / "config" / "cava-viz");
    check_int("dangling symlink ignored", Config::migrateLegacyDirs(), 0);
  }
}

int main() {
  printf("viz config test suite\n");
  printf("===========================\n");

  testRoundTrip();
  testCommentPreservation();
  testDefaults();
  testClampValidation();
  testMalformedFloat();
  testStateRoundTrip();
  testThemeIndexNotClamped();
  testAtomicSave();
  testSymlinkedConfig();
  testCliOverridesNotPersisted();
  testLegacyMigration();

  printf("\n===========================\n");
  printf("Results: %d/%d passed", tests_run - tests_failed, tests_run);
  if (tests_failed == 0)
    printf("  \033[32m— all good\033[0m\n");
  else
    printf("  \033[31m— %d FAILED\033[0m\n", tests_failed);

  return tests_failed ? 1 : 0;
}
