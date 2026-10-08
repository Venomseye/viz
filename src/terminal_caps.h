#pragma once
#include <functional>
#include <string>
#include <vector>

// Terminal colour capability decisions, kept free of ncurses so they can be
// unit-tested with a fake environment.

/// How viz should colour the bars.
///   Auto       detect (default)
///   TrueColor  redefine palette entries to exact RGB (needs a capable
///   terminal) Palette256 never redefine colours; map the gradient to the fixed
///   xterm-256 palette Basic      the 8 ANSI colours only
enum class ColorMode { Auto, TrueColor, Palette256, Basic };

/// Parse "auto", "truecolor"/"24bit", "256", "basic"/"16"/"8".  False if
/// unknown.
bool parseColorMode(const std::string &text, ColorMode &out);
const char *colorModeName(ColorMode m);

/// Environment lookup, injectable for tests (defaults to getenv).
using EnvFn = std::function<const char *(const char *)>;

/// True if `term` has a compiled terminfo entry in any directory ncurses would
/// search ($TERMINFO, ~/.terminfo, $TERMINFO_DIRS, /etc, /lib, /usr/share,
/// /usr/lib terminfo).  Forcing TERM to a name with no entry makes initscr()
/// fail ("Error opening terminal"), so check first.
bool terminfoEntryExists(const std::string &term, const EnvFn &env = nullptr);

/// True if the environment says the terminal handles 24-bit colour
/// (COLORTERM, Konsole, Kitty, VTE, Windows Terminal, iTerm2).
bool detectTruecolor(const EnvFn &env = nullptr);

/// The TERM value to set before initscr(), or "" to leave TERM alone.
/// Only ever returns "xterm-direct", and only when ALL of these hold:
///   * mode is Auto or TrueColor (Palette256/Basic never touch TERM)
///   * truecolor is detected (or mode == TrueColor)
///   * current TERM is a plain xterm/screen/*256color name
///   * the xterm-direct terminfo entry actually exists on this machine
std::string
termOverride(ColorMode mode, const EnvFn &env = nullptr,
             const std::function<bool(const std::string &)> &exists = nullptr);
