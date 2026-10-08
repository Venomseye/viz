#include "terminal_caps.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <vector>

static const char *realGetenv(const char *k) { return std::getenv(k); }

bool parseColorMode(const std::string &text, ColorMode &out) {
  std::string t;
  for (char c : text)
    t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (t == "auto")
    out = ColorMode::Auto;
  else if (t == "truecolor" || t == "24bit" || t == "rgb")
    out = ColorMode::TrueColor;
  else if (t == "256")
    out = ColorMode::Palette256;
  else if (t == "basic" || t == "16" || t == "8")
    out = ColorMode::Basic;
  else
    return false;
  return true;
}

const char *colorModeName(ColorMode m) {
  switch (m) {
  case ColorMode::Auto:
    return "auto";
  case ColorMode::TrueColor:
    return "truecolor";
  case ColorMode::Palette256:
    return "256";
  case ColorMode::Basic:
    return "basic";
  }
  return "auto";
}

static bool isFile(const std::string &p) {
  struct stat st{};
  return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static bool entryInDir(const std::string &dir, const std::string &term) {
  if (dir.empty() || term.empty())
    return false;
  // ncurses layouts: <dir>/x/xterm-direct  and  <dir>/78/xterm-direct (hex).
  static const char *hex = "0123456789abcdef";
  const unsigned char c = static_cast<unsigned char>(term[0]);
  std::string hexdir;
  hexdir += hex[c >> 4];
  hexdir += hex[c & 15];
  return isFile(dir + "/" + term[0] + "/" + term) ||
         isFile(dir + "/" + hexdir + "/" + term);
}

bool terminfoEntryExists(const std::string &term, const EnvFn &envIn) {
  const EnvFn env = envIn ? envIn : EnvFn(realGetenv);
  // Names are used to build paths: refuse anything that could escape a dir.
  if (term.empty() || term.find('/') != std::string::npos ||
      term.find("..") != std::string::npos)
    return false;

  std::vector<std::string> dirs;
  if (const char *d = env("TERMINFO"))
    dirs.push_back(d);
  if (const char *h = env("HOME"))
    dirs.push_back(std::string(h) + "/.terminfo");
  if (const char *td = env("TERMINFO_DIRS")) {
    std::string s = td;
    std::size_t pos = 0;
    while (pos <= s.size()) {
      std::size_t e = s.find(':', pos);
      if (e == std::string::npos)
        e = s.size();
      std::string part = s.substr(pos, e - pos);
      dirs.push_back(part.empty() ? "/usr/share/terminfo" : part);
      pos = e + 1;
    }
  }
  for (const char *d : {"/etc/terminfo", "/lib/terminfo", "/usr/share/terminfo",
                        "/usr/lib/terminfo", "/usr/local/share/terminfo"})
    dirs.push_back(d);

  for (const auto &d : dirs)
    if (entryInDir(d, term))
      return true;
  return false;
}

bool detectTruecolor(const EnvFn &envIn) {
  const EnvFn env = envIn ? envIn : EnvFn(realGetenv);
  if (const char *ct = env("COLORTERM"))
    if (!std::strcmp(ct, "truecolor") || !std::strcmp(ct, "24bit"))
      return true;
  for (const char *k : {"KONSOLE_VERSION", "KITTY_WINDOW_ID", "VTE_VERSION",
                        "WT_SESSION", "ITERM_SESSION_ID"})
    if (env(k))
      return true;
  return false;
}

std::string
termOverride(ColorMode mode, const EnvFn &envIn,
             const std::function<bool(const std::string &)> &exists) {
  const EnvFn env = envIn ? envIn : EnvFn(realGetenv);
  if (mode == ColorMode::Palette256 || mode == ColorMode::Basic)
    return "";
  if (mode == ColorMode::Auto && !detectTruecolor(env))
    return "";
  const char *term = env("TERM");
  if (!term)
    return "";
  const bool plain = std::strstr(term, "256color") != nullptr ||
                     std::strcmp(term, "xterm") == 0 ||
                     std::strcmp(term, "screen") == 0;
  if (!plain)
    return "";
  const bool have = exists ? exists("xterm-direct")
                           : terminfoEntryExists("xterm-direct", env);
  return have ? "xterm-direct" : "";
}
