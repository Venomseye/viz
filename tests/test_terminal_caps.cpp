// Tests for colour-capability decisions (no terminal or ncurses needed).

#include "terminal_caps.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
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

using Env = std::map<std::string, std::string>;
static EnvFn fake(const Env &e) {
  return [&e](const char *k) -> const char * {
    auto it = e.find(k);
    return it == e.end() ? nullptr : it->second.c_str();
  };
}

int main() {
  // ── parseColorMode ──────────────────────────────────────────────────────
  ColorMode m = ColorMode::Auto;
  CHECK(parseColorMode("auto", m) && m == ColorMode::Auto, "auto");
  CHECK(parseColorMode("truecolor", m) && m == ColorMode::TrueColor,
        "truecolor");
  CHECK(parseColorMode("24BIT", m) && m == ColorMode::TrueColor,
        "24bit, any case");
  CHECK(parseColorMode("256", m) && m == ColorMode::Palette256, "256");
  CHECK(parseColorMode("basic", m) && m == ColorMode::Basic, "basic");
  CHECK(parseColorMode("16", m) && m == ColorMode::Basic, "16 = basic");
  CHECK(!parseColorMode("rainbow", m), "unknown rejected");
  CHECK(!parseColorMode("", m), "empty rejected");

  // ── detectTruecolor ─────────────────────────────────────────────────────
  {
    Env e;
    CHECK(!detectTruecolor(fake(e)), "empty env: not truecolor");
    e = {{"COLORTERM", "truecolor"}};
    CHECK(detectTruecolor(fake(e)), "COLORTERM=truecolor");
    e = {{"COLORTERM", "24bit"}};
    CHECK(detectTruecolor(fake(e)), "COLORTERM=24bit");
    e = {{"COLORTERM", "yes"}};
    CHECK(!detectTruecolor(fake(e)), "COLORTERM=yes is not enough");
    for (const char *k : {"KONSOLE_VERSION", "KITTY_WINDOW_ID", "VTE_VERSION",
                          "WT_SESSION", "ITERM_SESSION_ID"}) {
      Env x = {{k, "1"}};
      CHECK(detectTruecolor(fake(x)), "terminal-specific variable detected");
    }
  }

  // ── termOverride ────────────────────────────────────────────────────────
  const auto yes = [](const std::string &) { return true; };
  const auto no = [](const std::string &) { return false; };
  {
    Env konsole = {{"TERM", "xterm-256color"}, {"KONSOLE_VERSION", "230800"}};
    CHECK(termOverride(ColorMode::Auto, fake(konsole), yes) == "xterm-direct",
          "Konsole + xterm-256color + entry present -> override (existing "
          "behaviour)");
    // THE CRASH CASE: forcing a TERM with no terminfo entry kills initscr().
    CHECK(termOverride(ColorMode::Auto, fake(konsole), no).empty(),
          "entry missing -> NO override (would make initscr fail)");

    Env vte = {{"TERM", "xterm"}, {"VTE_VERSION", "7600"}};
    CHECK(termOverride(ColorMode::Auto, fake(vte), yes) == "xterm-direct",
          "plain xterm on VTE");
    Env scr = {{"TERM", "screen"}, {"COLORTERM", "truecolor"}};
    CHECK(termOverride(ColorMode::Auto, fake(scr), yes) == "xterm-direct",
          "screen + COLORTERM");

    Env plain = {{"TERM", "xterm-256color"}};
    CHECK(termOverride(ColorMode::Auto, fake(plain), yes).empty(),
          "auto: no truecolor evidence -> leave TERM alone");
    CHECK(termOverride(ColorMode::TrueColor, fake(plain), yes) ==
              "xterm-direct",
          "--color=truecolor forces it even without evidence");
    CHECK(termOverride(ColorMode::TrueColor, fake(plain), no).empty(),
          "...but still never when the entry is missing");

    CHECK(termOverride(ColorMode::Palette256, fake(konsole), yes).empty(),
          "--color=256 never touches TERM");
    CHECK(termOverride(ColorMode::Basic, fake(konsole), yes).empty(),
          "--color=basic never touches TERM");

    Env linuxcon = {{"TERM", "linux"}, {"COLORTERM", "truecolor"}};
    CHECK(termOverride(ColorMode::Auto, fake(linuxcon), yes).empty(),
          "unrelated TERM (linux console) left alone");
    Env noterm = {{"COLORTERM", "truecolor"}};
    CHECK(termOverride(ColorMode::Auto, fake(noterm), yes).empty(),
          "TERM unset -> nothing to override");
    Env already = {{"TERM", "xterm-direct"}, {"COLORTERM", "truecolor"}};
    CHECK(termOverride(ColorMode::Auto, fake(already), yes).empty(),
          "already xterm-direct -> nothing to do");
  }

  // ── terminfoEntryExists against real directory layouts ──────────────────
  {
    char tmpl[] = "/tmp/viz_tinfo_XXXXXX";
    const std::string root = mkdtemp(tmpl);
    fs::create_directories(root + "/x");
    {
      std::ofstream(root + "/x/xterm-direct") << "stub";
    }
    fs::create_directories(root + "/78"); // hex layout (macOS ncurses)
    {
      std::ofstream(root + "/78/xterm-hex") << "stub";
    }

    Env e = {{"TERMINFO", root}};
    CHECK(terminfoEntryExists("xterm-direct", fake(e)),
          "found via $TERMINFO (letter dir)");
    CHECK(terminfoEntryExists("xterm-hex", fake(e)), "found via hex-named dir");
    CHECK(!terminfoEntryExists("no-such-term", fake(e)),
          "missing entry not found");

    Env d = {{"TERMINFO_DIRS", "/nonexistent:" + root}};
    CHECK(terminfoEntryExists("xterm-direct", fake(d)),
          "found via $TERMINFO_DIRS list");
    Env h = {{"HOME", "/nonexistent-home"}, {"TERMINFO_DIRS", ""}};
    CHECK(!terminfoEntryExists("xterm-direct-zzz", fake(h)),
          "empty TERMINFO_DIRS element is harmless");

    // Path-escape attempts are refused outright.
    CHECK(!terminfoEntryExists("../x/xterm-direct", fake(e)), "rejects ..");
    CHECK(!terminfoEntryExists("x/xterm-direct", fake(e)), "rejects /");
    CHECK(!terminfoEntryExists("", fake(e)), "rejects empty");
    fs::remove_all(root);
  }

  std::printf("terminal_caps: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
