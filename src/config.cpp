#include "config.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

static std::string xdgBase(const char *var, const char *suffix) {
  const char *x = std::getenv(var);
  if (x && x[0])
    return std::string(x);
  const char *h = std::getenv("HOME");
  if (h && h[0])
    return std::string(h) + suffix;
  return "/tmp";
}
static void mkdirFor(const std::string &path) {
  // Extract the directory component and create every intermediate level.
  // A plain mkdir() call only creates one level; on a fresh system
  // ~/.config might not exist, so we walk all components.
  const std::string dir = path.substr(0, path.rfind('/'));
  for (std::size_t pos = 1; pos < dir.size(); ++pos) {
    if (dir[pos] == '/') {
      mkdir(dir.substr(0, pos).c_str(), 0755); // ignore EEXIST
    }
  }
  mkdir(dir.c_str(), 0755); // final component
}

// ── Atomic file write ────────────────────────────────────────────────────────
// Writes to a temp file in the same directory, fsyncs, then rename()s over the
// target.  A crash or a concurrent reader can therefore never see a truncated
// config (the old fopen("w") truncated in place).  If `path` is a symlink
// (dotfile managers such as stow/chezmoi) the TARGET is replaced and the
// symlink is preserved; existing permission bits are kept.
static bool writeFileAtomic(const std::string &path,
                            const std::string &content) {
  std::string target = path;
  if (char *rp = realpath(path.c_str(), nullptr)) {
    target = rp;
    std::free(rp);
  }
  mode_t mode = 0644;
  bool had_mode = false;
  struct stat st{};
  if (stat(target.c_str(), &st) == 0) {
    mode = st.st_mode & 07777;
    had_mode = true;
  }

  const std::string tmp = target + ".tmp." + std::to_string(getpid());
  const int fd =
      open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
  if (fd < 0)
    return false;

  bool ok = true;
  const char *data = content.data();
  std::size_t left = content.size();
  while (left > 0) {
    const ssize_t n = write(fd, data, left);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      ok = false;
      break;
    }
    data += n;
    left -= static_cast<std::size_t>(n);
  }
  if (ok && had_mode)
    ok = (fchmod(fd, mode) == 0);
  if (ok)
    ok = (fsync(fd) == 0);
  if (close(fd) != 0)
    ok = false;
  if (ok)
    ok = (rename(tmp.c_str(), target.c_str()) == 0);
  if (!ok)
    unlink(tmp.c_str());
  return ok;
}

static bool readWholeFile(const std::string &path, std::string &out) {
  FILE *f = std::fopen(path.c_str(), "r");
  if (!f)
    return false;
  out.clear();
  char buf[4096];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
    out.append(buf, n);
  std::fclose(f);
  return true;
}

// True if `file_val` (text found in the config) already means the same as
// `new_val` (freshly formatted from the runtime value).  Lets save() leave a
// line — including the user's spacing and inline comment — untouched unless
// the setting really changed.  "1.0" == "1.00"; differences smaller than half
// a unit in new_val's last decimal place count as equal.
static bool sameValue(const std::string &file_val, const std::string &new_val) {
  if (file_val == new_val)
    return true;
  char *e1 = nullptr, *e2 = nullptr;
  const double a = std::strtod(file_val.c_str(), &e1);
  const double b = std::strtod(new_val.c_str(), &e2);
  if (e1 == file_val.c_str() || e2 == new_val.c_str() || *e2 != '\0')
    return false;
  // The file value may carry trailing spaces and an inline "# comment".
  while (*e1 == ' ' || *e1 == '\t')
    ++e1;
  if (*e1 != '\0' && *e1 != '#')
    return false;
  const std::size_t dot = new_val.find('.');
  const int decimals = (dot == std::string::npos)
                           ? 0
                           : static_cast<int>(new_val.size() - dot - 1);
  double tol = 0.5;
  for (int i = 0; i < decimals; ++i)
    tol /= 10.0;
  return std::fabs(a - b) <= tol + 1e-9;
}

// Digest of the bytes most recently written (or found already up to date) by
// Config::save() in this process.
static std::size_t g_last_saved_digest = 0;

std::size_t Config::lastSavedDigest() { return g_last_saved_digest; }

std::size_t Config::currentFileDigest() {
  std::string s;
  if (!readWholeFile(configPath(), s))
    return 0;
  return std::hash<std::string>{}(s);
}

void Config::inheritCliOverrides(const Config &prev) {
  if (prev.cli_theme) {
    theme = prev.theme;
    cli_theme = true;
  }
  if (prev.cli_fps) {
    fps = prev.fps;
    cli_fps = true;
  }
}

std::string Config::configPath() {
  return xdgBase("XDG_CONFIG_HOME", "/.config") + "/viz/config";
}
std::string Config::statePath() {
  return xdgBase("XDG_STATE_HOME", "/.local/state") + "/viz/state";
}

// ── One-time migration from the pre-rename "cava-viz" directories ───────────
// Moves <base>/cava-viz -> <base>/viz when the new directory does not exist
// yet.  rename() on a directory in the same parent is atomic and keeps its
// contents (config, themes/, state) and permissions; a symlinked directory
// (dotfile managers) moves as the symlink.  If the new directory already
// exists, nothing is touched: it always wins.
static bool moveLegacyDir(const std::string &base) {
  const std::string from = base + "/cava-viz";
  const std::string to = base + "/viz";
  struct stat st{};
  if (lstat(to.c_str(), &st) == 0)
    return false; // new location already present
  if (stat(from.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
    return false; // nothing to migrate (or a dangling symlink)
  return rename(from.c_str(), to.c_str()) == 0;
}

int Config::migrateLegacyDirs() {
  int moved = 0;
  moved += moveLegacyDir(xdgBase("XDG_CONFIG_HOME", "/.config")) ? 1 : 0;
  moved += moveLegacyDir(xdgBase("XDG_STATE_HOME", "/.local/state")) ? 1 : 0;
  return moved;
}

static bool parseKV(const char *line, char key[64], char val[1024]) {
  if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
    return false;
  if (std::sscanf(line, " %63[^=] = %1023[^\n]", key, val) != 2)
    return false;
  for (int i = static_cast<int>(strlen(key)) - 1; i >= 0 && key[i] == ' '; --i)
    key[i] = '\0';
  for (int i = static_cast<int>(strlen(val)) - 1;
       i >= 0 && (val[i] == ' ' || val[i] == '\r'); --i)
    val[i] = '\0';
  return key[0] != '\0';
}
static bool asBool(const char *v) {
  return strcmp(v, "1") == 0 || strcmp(v, "true") == 0;
}

// Parse a float config value using strtof so malformed values produce a
// diagnostic instead of silently returning 0.0 like atof would.
// Returns `fallback` on error so the field keeps its default.
static float parseFloat(const char *key, const char *val, float fallback) {
  char *end = nullptr;
  errno = 0;
  const float f = std::strtof(val, &end);
  // Skip any trailing whitespace that parseKV may have left.
  while (end && *end && std::isspace(static_cast<unsigned char>(*end)))
    ++end;
  if (!end || end == val || *end != '\0' || errno != 0) {
    std::fprintf(stderr,
                 "viz: config: '%s' expects a number, got '%s' — using %.3f\n",
                 key, val, static_cast<double>(fallback));
    return fallback;
  }
  return f;
}

bool Config::load() {
  FILE *f = std::fopen(configPath().c_str(), "r");
  if (!f)
    return false;
  char line[1536], k[64], v[1024];
  while (std::fgets(line, sizeof(line), f)) {
    if (!parseKV(line, k, v))
      continue;
    // Visual
    if (!strcmp(k, "theme"))
      theme = std::atoi(v);
    else if (!strcmp(k, "bar_width"))
      bar_width = std::atoi(v);
    else if (!strcmp(k, "gap_width"))
      gap_width = std::atoi(v);
    else if (!strcmp(k, "hud_pinned"))
      hud_pinned = asBool(v);
    // Rendering modes
    else if (!strcmp(k, "colour_cycle"))
      colour_cycle = asBool(v);
    else if (!strcmp(k, "per_bar_colour"))
      per_bar_colour = asBool(v);
    // Audio
    else if (!strcmp(k, "stereo"))
      stereo = asBool(v);
    else if (!strcmp(k, "high_cutoff"))
      high_cutoff = std::atoi(v);
    // FFT / Smoothing
    else if (!strcmp(k, "gravity"))
      gravity = parseFloat("gravity", v, gravity);
    else if (!strcmp(k, "monstercat"))
      monstercat = parseFloat("monstercat", v, monstercat);
    else if (!strcmp(k, "rise_factor"))
      rise_factor = parseFloat("rise_factor", v, rise_factor);
    else if (!strcmp(k, "bass_smooth"))
      bass_smooth = parseFloat("bass_smooth", v, bass_smooth);
    // Audio processing
    else if (!strcmp(k, "a_weighting"))
      a_weighting = asBool(v);
    else if (!strcmp(k, "noise_gate"))
      noise_gate = parseFloat("noise_gate", v, noise_gate);
    else if (!strcmp(k, "auto_mono"))
      auto_mono = asBool(v);
    // Sensitivity
    else if (!strcmp(k, "sensitivity"))
      sensitivity = parseFloat("sensitivity", v, sensitivity);
    else if (!strcmp(k, "auto_sens"))
      auto_sens = asBool(v);
    // Performance
    else if (!strcmp(k, "fps"))
      fps = std::atoi(v);
  }
  std::fclose(f);

#define CW(field, lo, hi)                                                      \
  {                                                                            \
    auto _c = std::clamp(field, (lo), (hi));                                   \
    if (_c != field)                                                           \
      std::fprintf(stderr, "viz: '%s' out of range, clamped.\n", #field);      \
    field = _c;                                                                \
  }
  // theme: only enforce non-negative; the upper bound depends on how many
  // user themes are loaded at runtime and is enforced by setThemeIdx().
  if (theme < 0)
    theme = 0;
  CW(bar_width, 1, 8)
  CW(gap_width, 0, 2)
  CW(high_cutoff, 1000, 24000)
  CW(gravity, 0.1f, 5.0f)
  CW(monstercat, 0.0f, 5.0f)
  CW(rise_factor, 0.0f, 0.95f)
  CW(bass_smooth, 0.0f, 1.0f)
  CW(noise_gate, 0.0f, 0.2f)
  CW(sensitivity, 0.2f, 8.0f)
  CW(fps, 10, 240)
#undef CW
  return true;
}

void Config::save() const {
  const std::string p = configPath();
  mkdirFor(p);

  // ── Key→value table (insertion order preserved for append path) ───────────
  using KV = std::pair<std::string, std::string>;

  auto fmtf = [](float v, int prec) -> std::string {
    char buf[32];
    std::snprintf(buf, sizeof(buf), prec == 3 ? "%.3f" : "%.2f",
                  static_cast<double>(v));
    return buf;
  };

  // A session-only CLI override must never reach the file.  When the file has
  // to be created from scratch, write the built-in default for that key.
  const Config defaults;
  const int theme_out = cli_theme ? defaults.theme : theme;
  const int fps_out = cli_fps ? defaults.fps : fps;

  const std::vector<KV> kvs = {
      {"theme", std::to_string(theme_out)},
      {"bar_width", std::to_string(bar_width)},
      {"gap_width", std::to_string(gap_width)},
      {"hud_pinned", hud_pinned ? "1" : "0"},
      {"colour_cycle", colour_cycle ? "1" : "0"},
      {"per_bar_colour", per_bar_colour ? "1" : "0"},
      {"stereo", stereo ? "1" : "0"},
      {"high_cutoff", std::to_string(high_cutoff)},
      {"gravity", fmtf(gravity, 2)},
      {"monstercat", fmtf(monstercat, 2)},
      {"rise_factor", fmtf(rise_factor, 2)},
      {"bass_smooth", fmtf(bass_smooth, 2)},
      {"a_weighting", a_weighting ? "1" : "0"},
      {"noise_gate", fmtf(noise_gate, 3)},
      {"auto_mono", auto_mono ? "1" : "0"},
      {"sensitivity", fmtf(sensitivity, 2)},
      {"auto_sens", auto_sens ? "1" : "0"},
      {"fps", std::to_string(fps_out)},
  };

  // ── Try to read existing file ─────────────────────────────────────────────
  std::vector<std::string> lines;
  std::string old_text;
  if (readWholeFile(p, old_text)) {
    std::size_t pos = 0;
    while (pos < old_text.size()) {
      std::size_t nl = old_text.find('\n', pos);
      nl = (nl == std::string::npos) ? old_text.size() : nl + 1;
      lines.push_back(old_text.substr(pos, nl - pos));
      pos = nl;
    }
  }

  // Final bytes to store; written once, atomically, at the end.
  std::string content;

  if (!lines.empty()) {
    // ── Read-modify-write: update known keys in-place ─────────────────────
    // Unknown lines (comments, blank lines, user additions) are kept as-is,
    // and so are key=value lines whose value did not change (spacing and
    // inline comments included).  An inline comment is only lost when that
    // line's value is actually rewritten.
    std::vector<bool> written(kvs.size(), false);

    for (auto &line : lines) {
      char k[64], vbuf[1024];
      if (!parseKV(line.c_str(), k, vbuf))
        continue;
      for (std::size_t i = 0; i < kvs.size(); ++i) {
        if (kvs[i].first == k) {
          // Keep the user's line verbatim unless the value really changed,
          // and never touch a key that is overridden on the command line.
          const bool pinned_by_cli = (kvs[i].first == "theme" && cli_theme) ||
                                     (kvs[i].first == "fps" && cli_fps);
          if (!pinned_by_cli && !sameValue(vbuf, kvs[i].second))
            line = kvs[i].first + " = " + kvs[i].second + "\n";
          written[i] = true;
          break;
        }
      }
    }

    // Append keys that were not present in the existing file
    for (std::size_t i = 0; i < kvs.size(); ++i) {
      if (!written[i])
        lines.push_back(kvs[i].first + " = " + kvs[i].second + "\n");
    }

    for (const auto &l : lines)
      content += l;

    // Nothing changed (e.g. a key press that doesn't alter any setting):
    // don't touch the file, so no inotify event and no pointless reload.
    if (content == old_text) {
      g_last_saved_digest = std::hash<std::string>{}(content);
      return;
    }
    if (writeFileAtomic(p, content))
      g_last_saved_digest = std::hash<std::string>{}(content);
    return;
  }

  // ── First-time write: emit the full annotated template ────────────────────
  char *mbuf = nullptr;
  std::size_t mlen = 0;
  FILE *f = open_memstream(&mbuf, &mlen);
  if (!f)
    return;

  fprintf(f, "# viz configuration\n");
  fprintf(f, "# Edit while running — inotify reloads changes instantly.\n\n");

  fprintf(
      f,
      "# ── Visual ──────────────────────────────────────────────────────\n");
  fprintf(f, "# 0=Fire 1=Plasma 2=Neon 3=Teal 4=Sunset 5=Candy\n");
  fprintf(f, "# 6=Aurora 7=Inferno 8=White 9=Rose 10=Mermaid 11=Vapor\n");
  fprintf(f, "theme          = %d\n", theme_out);
  fprintf(f, "bar_width      = %d\n", bar_width);
  fprintf(f, "gap_width      = %d\n", gap_width);
  fprintf(f, "hud_pinned     = %d\n", hud_pinned ? 1 : 0);

  fprintf(f, "\n# ── Rendering modes "
             "──────────────────────────────────────────────\n");
  fprintf(f, "# colour_cycle: slowly rotate gradient hue over time\n");
  fprintf(f, "colour_cycle   = %d\n", colour_cycle ? 1 : 0);
  fprintf(
      f, "# per_bar_colour: map colour to bar index (bass=base, treble=tip)\n");
  fprintf(f, "per_bar_colour = %d\n", per_bar_colour ? 1 : 0);

  fprintf(f, "\n# ── Audio "
             "────────────────────────────────────────────────────────\n");
  fprintf(f, "stereo         = %d\n", stereo ? 1 : 0);
  fprintf(f, "high_cutoff    = %d\n", high_cutoff);

  fprintf(f, "\n# ── FFT / Smoothing "
             "──────────────────────────────────────────────\n");
  fprintf(f, "# gravity: fall speed (0.1=slow, 1.0=default, 5.0=instant)\n");
  fprintf(f, "gravity        = %.2f\n", static_cast<double>(gravity));
  fprintf(f, "# monstercat: bar spread (0=off, 1.0-5.0; values in (0,1) act as "
             "1.0; 1.5=default)\n");
  fprintf(f, "monstercat     = %.2f\n", static_cast<double>(monstercat));
  fprintf(f, "# rise_factor: attack smoothing (0.0=instant, 0.95=very slow)\n");
  fprintf(f, "rise_factor    = %.2f\n", static_cast<double>(rise_factor));
  fprintf(f, "# bass_smooth: extra smoothing for bass bars (0.0=off, 0.1-0.3 "
             "recommended)\n");
  fprintf(f, "bass_smooth    = %.2f\n", static_cast<double>(bass_smooth));

  fprintf(f, "\n# ── Audio processing "
             "─────────────────────────────────────────────\n");
  fprintf(f, "# a_weighting: IEC 61672 perceptual frequency weighting\n");
  fprintf(f, "a_weighting    = %d\n", a_weighting ? 1 : 0);
  fprintf(f,
          "# noise_gate: bars below this (post-sens) snap to zero (0.0-0.2)\n");
  fprintf(f, "noise_gate     = %.3f\n", static_cast<double>(noise_gate));
  fprintf(
      f,
      "# auto_mono: collapse stereo to mono when L/R are nearly identical\n");
  fprintf(f, "auto_mono      = %d\n", auto_mono ? 1 : 0);

  fprintf(f, "\n# ── Sensitivity "
             "──────────────────────────────────────────────────\n");
  fprintf(f, "sensitivity    = %.2f\n", static_cast<double>(sensitivity));
  fprintf(f, "auto_sens      = %d\n", auto_sens ? 1 : 0);

  fprintf(f, "\n# ── Performance "
             "──────────────────────────────────────────────────\n");
  fprintf(f, "fps            = %d\n", fps_out);

  std::fclose(f); // flushes into mbuf/mlen
  content.assign(mbuf ? mbuf : "", mlen);
  std::free(mbuf);
  if (writeFileAtomic(p, content))
    g_last_saved_digest = std::hash<std::string>{}(content);
}

bool Config::loadState() {
  FILE *f = std::fopen(statePath().c_str(), "r");
  if (!f)
    return false;
  char line[1536], k[64], v[1024];
  while (std::fgets(line, sizeof(line), f)) {
    if (!parseKV(line, k, v))
      continue;
    if (!strcmp(k, "last_source"))
      last_source = v;
  }
  std::fclose(f);
  return true;
}
void Config::saveState() const {
  const std::string p = statePath();
  mkdirFor(p);
  writeFileAtomic(p, "# viz internal state — do not edit\nlast_source = " +
                         last_source + "\n");
}
