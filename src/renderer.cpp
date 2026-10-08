#include "renderer.h"

#include "beat_detect.h"
#include "text_utils.h"
#include "user_theme.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <locale.h>

// Unicode eighth-block characters ▁–█ for sub-cell bar tips.
static const char *kBlk[9] = {
    " ",
    "\xe2\x96\x81",
    "\xe2\x96\x82",
    "\xe2\x96\x83",
    "\xe2\x96\x84",
    "\xe2\x96\x85",
    "\xe2\x96\x86",
    "\xe2\x96\x87",
    "\xe2\x96\x88",
};
static constexpr int SUB = 8;

static const char *kThemeNames[static_cast<int>(Theme::COUNT)] = {
    "Fire",   "Plasma",  "Neon",  "Teal", "Sunset",  "Candy",
    "Aurora", "Inferno", "White", "Rose", "Mermaid", "Vapor",
};

const char *builtinThemeName(int idx) noexcept {
  if (idx < 0 || idx >= static_cast<int>(Theme::COUNT))
    return "";
  return kThemeNames[idx];
}

struct RGB {
  short r, g, b;
};
struct Stop {
  float t;
  RGB c;
};

// ── Gradient palette
// ──────────────────────────────────────────────────────────
//
// 7 stops per theme.  Initialised as  { t, {r, g, b} }  (no extra braces
// around the scalar t — avoids -Wbraced-scalar-init warnings).
//
// Each theme follows a clean HSV arc so adjacent terminal rows have a small
// per-row Δ (< 80 ncurses units) — the minimum achievable without sub-pixel
// rendering.  GAMMA = 1.0 (linear palette) distributes the variation evenly
// across all rows.  smoothstep() inside sampleTheme() makes each stop
// interval C1-continuous, eliminating slope-kink banding at stop boundaries.
//
static const Stop kGrad[static_cast<int>(Theme::COUNT)][7] = {
    // FIRE  vivid-orange → orange-red → red → rose-red → hot-pink
    {{0.00f, {1000, 467, 0}},
     {0.17f, {1000, 267, 0}},
     {0.33f, {1000, 67, 0}},
     {0.50f, {980, 0, 163}},
     {0.67f, {940, 0, 345}},
     {0.83f, {900, 0, 525}},
     {1.00f, {860, 0, 659}}},

    // PLASMA  blue-violet → electric-blue → neon-cyan
    {{0.00f, {301, 0, 820}},
     {0.17f, {117, 0, 880}},
     {0.33f, {0, 93, 930}},
     {0.50f, {0, 356, 970}},
     {0.67f, {30, 612, 1000}},
     {0.83f, {70, 814, 1000}},
     {1.00f, {100, 1000, 1000}}},

    // NEON  electric-magenta → violet → electric-blue
    {{0.00f, {920, 0, 767}},
     {0.17f, {918, 0, 950}},
     {0.33f, {744, 0, 970}},
     {0.50f, {567, 0, 1000}},
     {0.67f, {333, 0, 1000}},
     {0.83f, {67, 0, 1000}},
     {1.00f, {0, 200, 1000}}},

    // TEAL  dark-teal → teal → bright-aqua
    {{0.00f, {84, 700, 618}},
     {0.17f, {77, 770, 712}},
     {0.33f, {66, 830, 805}},
     {0.50f, {45, 876, 890}},
     {0.67f, {28, 879, 940}},
     {0.83f, {10, 867, 980}},
     {1.00f, {0, 800, 1000}}},

    // SUNSET  amber → orange-red → crimson → deep-violet-pink
    {{0.00f, {1000, 583, 0}},
     {0.17f, {1000, 367, 0}},
     {0.33f, {1000, 133, 0}},
     {0.50f, {980, 0, 98}},
     {0.67f, {950, 0, 317}},
     {0.83f, {920, 0, 552}},
     {1.00f, {900, 0, 750}}},

    // CANDY  hot-rose → magenta → violet
    {{0.00f, {1000, 0, 333}},
     {0.17f, {1000, 0, 533}},
     {0.33f, {1000, 0, 700}},
     {0.50f, {1000, 0, 867}},
     {0.67f, {933, 0, 1000}},
     {0.83f, {733, 0, 1000}},
     {1.00f, {533, 0, 1000}}},

    // AURORA  sea-green → teal → sky-blue → soft-violet
    {{0.00f, {0, 780, 364}},
     {0.17f, {0, 840, 588}},
     {0.33f, {36, 900, 842}},
     {0.50f, {76, 775, 950}},
     {0.67f, {98, 568, 980}},
     {0.83f, {130, 362, 1000}},
     {1.00f, {216, 160, 1000}}},

    // INFERNO  blood-red → red-orange → amber → bright-gold
    {{0.00f, {760, 51, 0}},
     {0.17f, {820, 164, 0}},
     {0.33f, {880, 323, 0}},
     {0.50f, {930, 496, 0}},
     {0.67f, {970, 679, 0}},
     {0.83f, {1000, 867, 0}},
     {1.00f, {1000, 1000, 0}}},

    // WHITE  solid bright white — no gradient, matches cava's solid-colour mode
    {{0.00f, {1000, 1000, 1000}},
     {0.17f, {1000, 1000, 1000}},
     {0.33f, {1000, 1000, 1000}},
     {0.50f, {1000, 1000, 1000}},
     {0.67f, {1000, 1000, 1000}},
     {0.83f, {1000, 1000, 1000}},
     {1.00f, {1000, 1000, 1000}}},

    // ROSE  crimson-rose → coral → salmon → orchid-pink
    {{0.00f, {840, 84, 109}},
     {0.17f, {880, 123, 262}},
     {0.33f, {920, 166, 417}},
     {0.50f, {950, 228, 577}},
     {0.67f, {980, 294, 728}},
     {0.83f, {1000, 360, 893}},
     {1.00f, {962, 430, 1000}}},

    // MERMAID  ocean-blue → teal → periwinkle → orchid
    {{0.00f, {94, 528, 780}},
     {0.17f, {126, 388, 840}},
     {0.33f, {160, 258, 890}},
     {0.50f, {285, 186, 930}},
     {0.67f, {516, 213, 970}},
     {0.83f, {747, 240, 1000}},
     {1.00f, {976, 270, 1000}}},

    // VAPOR (synthwave)  deep-neon-purple → violet-magenta → hot-pink
    {{0.00f, {352, 0, 660}},
     {0.17f, {555, 0, 740}},
     {0.33f, {793, 0, 820}},
     {0.50f, {880, 0, 733}},
     {0.67f, {940, 0, 595}},
     {0.83f, {980, 0, 425}},
     {1.00f, {1000, 0, 267}}},
};

// ── Colour helpers
// ────────────────────────────────────────────────────────────
static RGB lerpRGB(RGB a, RGB b, float t) noexcept {
  return {
      static_cast<short>(a.r + (b.r - a.r) * t),
      static_cast<short>(a.g + (b.g - a.g) * t),
      static_cast<short>(a.b + (b.b - a.b) * t),
  };
}

// Cubic smoothstep — C1-continuous at stop boundaries, eliminates kink-bands.
static float smoothstep(float t) noexcept { return t * t * (3.0f - 2.0f * t); }

static RGB sampleTheme(int ti, float t) noexcept {
  const Stop *s = kGrad[ti];
  for (int i = 1; i < 7; ++i) {
    if (t <= s[i].t) {
      float lt = (t - s[i - 1].t) / (s[i].t - s[i - 1].t);
      return lerpRGB(s[i - 1].c, s[i].c, smoothstep(std::clamp(lt, 0.f, 1.f)));
    }
  }
  return s[6].c;
}

// Sample a user-defined gradient at position t ∈ [0, 1].
// Uses the same C1-continuous smoothstep as sampleTheme().
static RGB sampleUserTheme(const UserTheme &ut, float t) noexcept {
  const auto &s = ut.stops;
  if (s.empty())
    return {1000, 1000, 1000};
  if (t <= s.front().pos)
    return {s.front().r, s.front().g, s.front().b};
  if (t >= s.back().pos)
    return {s.back().r, s.back().g, s.back().b};
  for (std::size_t i = 1; i < s.size(); ++i) {
    if (t <= s[i].pos) {
      const float span = s[i].pos - s[i - 1].pos;
      const float lt = (span > 0.f) ? (t - s[i - 1].pos) / span : 0.f;
      const RGB a = {s[i - 1].r, s[i - 1].g, s[i - 1].b};
      const RGB b = {s[i].r, s[i].g, s[i].b};
      return lerpRGB(a, b, smoothstep(std::clamp(lt, 0.f, 1.f)));
    }
  }
  return {s.back().r, s.back().g, s.back().b};
}

// Map an ncurses-scale RGB (0–1000) to the nearest xterm-256 colour index.
// Used by the 256-colour fallback path for ALL themes (the old hand-written
// per-theme tables were 46/48 entries long, so the last two slots were colour
// 0 = black: bar tips and the FPS text vanished).
static short nearestXterm256(short r1k, short g1k, short b1k) noexcept {
  // Convert to 0–255 byte scale.
  const int r = static_cast<int>(r1k * 255 / 1000);
  const int g = static_cast<int>(g1k * 255 / 1000);
  const int b = static_cast<int>(b1k * 255 / 1000);

  // The 6×6×6 colour cube: levels 0, 95, 135, 175, 215, 255.
  static const int kLev[6] = {0, 95, 135, 175, 215, 255};
  auto nearest = [](int v) -> int {
    int bi = 0, bd = 1 << 30;
    for (int i = 0; i < 6; ++i) {
      const int d = (v - kLev[i]) * (v - kLev[i]);
      if (d < bd) {
        bd = d;
        bi = i;
      }
    }
    return bi;
  };
  const int ri = nearest(r), gi = nearest(g), bi = nearest(b);
  const short cube = static_cast<short>(16 + 36 * ri + 6 * gi + bi);

  // Also check the 24 xterm grayscale shades (indices 232–255, values 8–238).
  const int gray = (r + g + b) / 3;
  const int gi2 = std::clamp((gray - 8) / 10, 0, 23);
  const int gray_v = 8 + gi2 * 10;
  const short graysq = static_cast<short>(232 + gi2);

  const int dist_cube = (r - kLev[ri]) * (r - kLev[ri]) +
                        (g - kLev[gi]) * (g - kLev[gi]) +
                        (b - kLev[bi]) * (b - kLev[bi]);
  const int dist_gray = (r - gray_v) * (r - gray_v) +
                        (g - gray_v) * (g - gray_v) +
                        (b - gray_v) * (b - gray_v);
  return dist_cube <= dist_gray ? cube : graysq;
}

static RGB hsvToRgb(float h, float s, float v) noexcept {
  const float hh = std::fmod(h, 360.f) / 60.f;
  const int i = static_cast<int>(hh);
  const float ff = hh - i;
  const float p = v * (1 - s), q = v * (1 - s * ff),
              t_ = v * (1 - s * (1 - ff));
  float r = 0, g = 0, b = 0;
  switch (i) {
  case 0:
    r = v;
    g = t_;
    b = p;
    break;
  case 1:
    r = q;
    g = v;
    b = p;
    break;
  case 2:
    r = p;
    g = v;
    b = t_;
    break;
  case 3:
    r = p;
    g = q;
    b = v;
    break;
  case 4:
    r = t_;
    g = p;
    b = v;
    break;
  default:
    r = v;
    g = p;
    b = q;
    break;
  }
  return {static_cast<short>(r * 1000), static_cast<short>(g * 1000),
          static_cast<short>(b * 1000)};
}

// ── Colour capability setup ──────────────────────────────────────────────────
// Decided in terminal_caps.cpp (unit-tested).  After initscr(), work out which
// strategy the terminal really supports.
void Renderer::updateColorCaps() {
  const bool can_redefine = (COLORS >= 256) && can_change_color();
  switch (color_mode_) {
  case ColorMode::Palette256:
    can_rgb_ = false;
    basic_colors_ = (COLORS < 256);
    break;
  case ColorMode::Basic:
    can_rgb_ = false;
    basic_colors_ = true;
    break;
  case ColorMode::Auto:
  case ColorMode::TrueColor:
    can_rgb_ = can_redefine;
    basic_colors_ = (COLORS < 256);
    break;
  }
  invalidatePaletteCache();
}

Renderer::Renderer() { last_frame_tp_ = Clock::now(); }
Renderer::~Renderer() {
  if (initialized_)
    endwin();
}

void Renderer::applyNcursesSettings() {
  cbreak();
  noecho();
  curs_set(0);
  keypad(stdscr, TRUE);
  nodelay(stdscr, TRUE);
}

bool Renderer::init() {
  utf8_ok_ = initUtf8Locale(); // must precede initscr()
  set_escdelay(20);
  {
    // Only ever switches TERM to xterm-direct if that terminfo entry exists
    // (otherwise initscr() would fail outright); see terminal_caps.h.
    const std::string t = termOverride(color_mode_);
    if (!t.empty())
      setenv("TERM", t.c_str(), 1);
  }
  initscr();
  applyNcursesSettings();
  if (!has_colors()) {
    endwin();
    return false;
  }
  start_color();
  use_default_colors();
  updateColorCaps();
  grad_steps_ = GRAD_STEPS_MAX;
  rebuildColors();
  last_change_tp_ = Clock::now();
  last_frame_tp_ = Clock::now();
  initialized_ = true;
  return true;
}

void Renderer::handleResize() {
  endwin();
  refresh();
  clear();
  start_color();
  use_default_colors();
  updateColorCaps(); // also drops the palette cache: the terminal was reset
  grad_steps_ = GRAD_STEPS_MAX;
  applyNcursesSettings();
  rebuildColors();
  invalidatePrev();
  prev_grad_avail_ = 0;
  needs_clear_ = true;
  notifyChange();
}

// ── rebuildColors
// ─────────────────────────────────────────────────────────────
//
// Colour index safety:
//   COLORS = 256  →  indices 0..255.  We start at COLOR_BASE = 16 (after the
//   16 ANSI colours), leaving 240 slots.  Reserving HUD_PAIRS = 4 at the top
//   gives 236 usable gradient steps.
//   Constraint:  COLOR_BASE + grad_steps_ + HUD_PAIRS − 1 ≤ COLORS − 1
//   HUD colours are placed immediately after at +grad_steps_+[0..3].
//
// GAMMA = 1.0 (linear) gives the most uniform per-row colour step size,
// minimising worst-case banding.
//
// init_color() only when the slot's RGB actually changed since we last sent it.
void Renderer::setPaletteColor(short idx, short r, short g, short b) {
  const std::size_t base = static_cast<std::size_t>(idx) * 3;
  if (pal_cache_.size() < base + 3)
    pal_cache_.resize(base + 3, -1);
  if (pal_cache_[base] == r && pal_cache_[base + 1] == g &&
      pal_cache_[base + 2] == b)
    return;
  if (init_color(idx, r, g, b) == OK) {
    pal_cache_[base] = r;
    pal_cache_[base + 1] = g;
    pal_cache_[base + 2] = b;
  }
}

void Renderer::rebuildColors() {
  static constexpr float GAMMA = 1.00f;

  const int max_safe =
      std::max(4, COLORS - static_cast<int>(COLOR_BASE) - HUD_PAIRS);
  grad_steps_ = std::min(GRAD_STEPS_MAX, max_safe);

  // Determine whether we're rendering a built-in or a user theme.
  const bool is_user = (theme_abs_ >= static_cast<int>(Theme::COUNT));
  const int user_idx = theme_abs_ - static_cast<int>(Theme::COUNT);
  const int bi =
      is_user ? 0 : theme_abs_; // built-in index (only valid when !is_user)
  const bool has_user =
      is_user && (user_idx < static_cast<int>(user_themes_.size()));

  const float hue_a = colour_cycle_ ? 0.38f : 0.0f;

  // Helper: sample either gradient at position t.
  auto sample = [&](float t) -> RGB {
    if (has_user)
      return sampleUserTheme(user_themes_[user_idx], t);
    return sampleTheme(bi, t);
  };

  if (can_rgb_) {
    for (int i = 0; i < grad_steps_; ++i) {
      const float raw = static_cast<float>(i) / std::max(1, grad_steps_ - 1);
      const float t = std::pow(raw, GAMMA);
      RGB rgb = sample(t);
      if (hue_a > 0.f) {
        RGB hue = hsvToRgb(hue_offset_ + raw * 60.f, 0.82f, 0.92f);
        rgb = lerpRGB(rgb, hue, hue_a);
      }
      const short ci = static_cast<short>(COLOR_BASE + i);
      setPaletteColor(ci, rgb.r, rgb.g, rgb.b);
      init_pair(i + 1, ci, -1);
    }
  } else if (!basic_colors_) {
    // Same gradient as the truecolor path, quantised to the xterm-256 palette
    // (no init_color needed, so this also works on terminals that can't
    // redefine colours).
    for (int i = 0; i < grad_steps_; ++i) {
      const float raw = static_cast<float>(i) / std::max(1, grad_steps_ - 1);
      RGB rgb = sample(raw);
      if (hue_a > 0.f) {
        const RGB hue = hsvToRgb(hue_offset_ + raw * 60.f, 0.82f, 0.92f);
        rgb = lerpRGB(rgb, hue, hue_a);
      }
      init_pair(i + 1, nearestXterm256(rgb.r, rgb.g, rgb.b), -1);
    }
  } else {
    for (int i = 0; i < grad_steps_; ++i) {
      const float f = static_cast<float>(i) / std::max(1, grad_steps_ - 1);
      const short fg = f < 0.4f    ? COLOR_YELLOW
                       : f < 0.72f ? COLOR_RED
                                   : COLOR_WHITE;
      init_pair(i + 1, fg, -1);
    }
  }

  // HUD accent pairs — right after the gradient.
  static const float kHF[HUD_PAIRS] = {0.55f, 0.72f, 0.88f, 1.00f};
  for (int lv = 0; lv < HUD_PAIRS; ++lv) {
    const short hud_ci = static_cast<short>(COLOR_BASE + grad_steps_ + lv);
    if (can_rgb_) {
      RGB rgb = sample(kHF[lv]);
      setPaletteColor(hud_ci, rgb.r, rgb.g, rgb.b);
      init_pair(grad_steps_ + 1 + lv, hud_ci, -1);
    } else if (!basic_colors_) {
      const RGB rgb = sample(kHF[lv]);
      init_pair(grad_steps_ + 1 + lv, nearestXterm256(rgb.r, rgb.g, rgb.b), -1);
    } else {
      init_pair(grad_steps_ + 1 + lv,
                kHF[lv] < 0.75f ? COLOR_YELLOW : COLOR_WHITE, -1);
    }
  }
}

int Renderer::gradPair(float frac) const noexcept {
  const int idx = static_cast<int>(frac * std::max(1, grad_steps_ - 1) + 0.5f);
  return std::clamp(idx, 0, grad_steps_ - 1) + 1;
}

int Renderer::hudPair(int lv) const noexcept {
  return grad_steps_ + 1 + std::clamp(lv, 0, HUD_PAIRS - 1);
}

// ── Public helpers
// ────────────────────────────────────────────────────────────
int Renderer::cols() const { return COLS; }
int Renderer::rows() const { return LINES; }

int Renderer::barCount() const {
  const int unit = std::max(1, bar_w_ + gap_w_);
  return std::max(1, (COLS + gap_w_) / (2 * unit));
}

int Renderer::autoBarWidth() const {
  return std::clamp(COLS / 40, BAR_W_MIN, BAR_W_MAX);
}

void Renderer::notifyChange() {
  last_change_tp_ = Clock::now();
  hud_visible_ = true;
}

void Renderer::invalidatePrev() {
  prev_l_.assign(prev_l_.size(), -1);
  prev_r_.assign(prev_r_.size(), -1);
}

void Renderer::showFeedback(const std::string &msg) {
  feedback_msg_ = msg;
  feedback_tp_ = Clock::now();
  feedback_active_ = true;
  notifyChange();
}

void Renderer::setTheme(Theme t) {
  theme_abs_ =
      std::clamp(static_cast<int>(t), 0, static_cast<int>(Theme::COUNT) - 1);
  if (initialized_)
    rebuildColors();
  invalidatePrev();
  needs_clear_ = true;
}

void Renderer::setThemeIdx(int idx) {
  const int total =
      static_cast<int>(Theme::COUNT) + static_cast<int>(user_themes_.size());
  theme_abs_ = std::clamp(idx, 0, std::max(0, total - 1));
  if (initialized_)
    rebuildColors();
  invalidatePrev();
  needs_clear_ = true;
}

void Renderer::setUserThemes(std::vector<UserTheme> themes) {
  user_themes_ = std::move(themes);
  // Re-clamp in case the active index now points past the end of the list.
  const int total =
      static_cast<int>(Theme::COUNT) + static_cast<int>(user_themes_.size());
  if (theme_abs_ >= total)
    theme_abs_ = 0;
  if (initialized_)
    rebuildColors();
  invalidatePrev();
  needs_clear_ = true;
}

int Renderer::nextTheme() {
  const int total =
      static_cast<int>(Theme::COUNT) + static_cast<int>(user_themes_.size());
  theme_abs_ = (theme_abs_ + 1) % total;
  if (initialized_)
    rebuildColors();
  invalidatePrev();
  needs_clear_ = true;
  notifyChange();
  return theme_abs_;
}

Theme Renderer::theme() const {
  return (theme_abs_ < static_cast<int>(Theme::COUNT))
             ? (Theme)theme_abs_
             : Theme::COUNT; // sentinel: signals "user theme active"
}

std::string Renderer::themeName() const {
  if (theme_abs_ < static_cast<int>(Theme::COUNT))
    return kThemeNames[theme_abs_];
  const int ui = theme_abs_ - static_cast<int>(Theme::COUNT);
  if (ui < static_cast<int>(user_themes_.size()))
    return user_themes_[ui].name;
  return "?";
}

int Renderer::increaseBarWidth() {
  bar_w_ = std::min(bar_w_ + 1, BAR_W_MAX);
  invalidatePrev();
  needs_clear_ = true;
  showFeedback("Width: " + std::to_string(bar_w_));
  return bar_w_;
}

int Renderer::decreaseBarWidth() {
  bar_w_ = std::max(bar_w_ - 1, BAR_W_MIN);
  invalidatePrev();
  needs_clear_ = true;
  showFeedback("Width: " + std::to_string(bar_w_));
  return bar_w_;
}

void Renderer::setBarWidth(int w) {
  bar_w_ = std::clamp(w, BAR_W_MIN, BAR_W_MAX);
  invalidatePrev();
  needs_clear_ = true;
}

int Renderer::cycleGap() {
  gap_w_ = (gap_w_ >= 2) ? 0 : gap_w_ + 1;
  invalidatePrev();
  needs_clear_ = true;
  showFeedback("Gap: " + std::to_string(gap_w_));
  return gap_w_;
}

void Renderer::resetPrev(int n, int avail) {
  if (static_cast<int>(prev_l_.size()) != n || prev_avail_ != avail) {
    prev_l_.assign(n, -1);
    prev_r_.assign(n, -1);
    prev_avail_ = avail;
  }
}

// ── drawBarColumn
// ─────────────────────────────────────────────────────────────
void Renderer::drawBarColumn(int col, int bar_idx, int total_bars,
                             int height_sub, int prev_sub, int avail) {
  if (col < 0 || col + bar_w_ > COLS || avail <= 0)
    return;

  const bool force = (prev_sub < 0);
  if (!force && height_sub == prev_sub)
    return;

  const int prev_h = force ? 0 : prev_sub;
  const int cur_lh = height_sub / SUB;
  const int prev_lh = prev_h / SUB;

  int max_line = (std::max(height_sub, prev_h) + SUB) / SUB;
  max_line = std::min(max_line, avail);

  char lbuf[BAR_W_MAX * 3 + 1];

  for (int line = 0; line < max_line; ++line) {
    const int row = HUD_ROWS + avail - 1 - line;
    if (row < HUD_ROWS || row >= LINES)
      continue;

    // frac=0 → vivid base colour at bar bottom; frac=1 → tip colour.
    float frac;
    if (per_bar_colour_ && total_bars > 1)
      frac = static_cast<float>(bar_idx) / static_cast<float>(total_bars - 1);
    else
      frac = (avail > 1)
                 ? static_cast<float>(line) / static_cast<float>(avail - 1)
                 : 1.f;

    const int cp = gradPair(frac);
    const int attr = beat_flash_ ? (COLOR_PAIR(cp) | A_BOLD) : COLOR_PAIR(cp);

    if (height_sub >= line * SUB + 1) {
      int bar_step;
      if (cur_lh == line) {
        bar_step = height_sub % SUB;
        if (bar_step == 0)
          bar_step = SUB - 1;
        else
          bar_step--;
      } else if (force || prev_lh <= line) {
        bar_step = SUB - 1;
      } else {
        continue;
      }
      const char *blk = kBlk[std::clamp(bar_step + 1, 1, 8)];
      for (int w = 0; w < bar_w_; ++w)
        memcpy(lbuf + w * 3, blk, 3);
      lbuf[bar_w_ * 3] = '\0';
      attron(attr);
      mvaddstr(row, col, lbuf);
      attroff(attr);
    } else if (!force && prev_lh >= line) {
      mvhline(row, col, ' ', bar_w_);
    }
  }
}

// ── drawMirrorLR
// ──────────────────────────────────────────────────────────────
void Renderer::drawMirrorLR(const std::vector<float> &left,
                            const std::vector<float> &right, int avail) {
  if (COLS <= 0 || avail <= 0)
    return;

  const int n = static_cast<int>(left.size());
  const int unit = bar_w_ + gap_w_;

  const int total_w = 2 * n * unit - gap_w_;
  const int left_edge = (COLS - total_w) / 2;
  const int centre = left_edge + n * unit;

  resetPrev(n, avail);

  for (int b = 0; b < n; ++b) {
    const float rv = (b < static_cast<int>(right.size())) ? right[b] : 0.f;
    const float lv = (b < static_cast<int>(left.size())) ? left[b] : 0.f;
    const int h_sub_r =
        std::clamp(static_cast<int>(rv * avail * SUB), 0, avail * SUB);
    const int h_sub_l =
        std::clamp(static_cast<int>(lv * avail * SUB), 0, avail * SUB);

    const int rcol = centre + b * unit;
    if (rcol + bar_w_ <= COLS) {
      drawBarColumn(rcol, b, n, h_sub_r, prev_r_[b], avail);
      prev_r_[b] = h_sub_r;
    }
    const int lcol = centre - (b + 1) * unit;
    if (lcol >= 0) {
      drawBarColumn(lcol, b, n, h_sub_l, prev_l_[b], avail);
      prev_l_[b] = h_sub_l;
    }
  }
}

// ── drawStatusBar
// ─────────────────────────────────────────────────────────────
void Renderer::drawStatusBar(double fps, const std::string &backend, float sens,
                             bool auto_sens) {
  if (LINES < HUD_ROWS + 1 || COLS < 10)
    return;

  mvhline(0, 0, ' ', COLS);

  const bool show_fb =
      feedback_active_ &&
      (std::chrono::duration<double>(Clock::now() - feedback_tp_).count() <
       FEEDBACK_SECS);
  if (!show_fb)
    feedback_active_ = false;

  char lbuf[512];
  if (show_fb) {
    std::snprintf(lbuf, sizeof(lbuf), "  %s", feedback_msg_.c_str());
  } else {
    char flags[8] = {};
    int fi = 0;
    if (colour_cycle_)
      flags[fi++] = 'C';
    if (per_bar_colour_)
      flags[fi++] = 'B';
    flags[fi] = '\0';

    char src[80] = {};
    if (!source_name_.empty())
      std::snprintf(src, sizeof(src), "  %s",
                    truncateUtf8(source_name_, 38).c_str());

    std::snprintf(lbuf, sizeof(lbuf), " %s  %s  W:%d G:%d  Sens:%.1f%s%s%s",
                  backend.empty() ? "?" : backend.c_str(), themeName().c_str(),
                  bar_w_, gap_w_, static_cast<double>(sens),
                  auto_sens ? " A" : "", hud_pinned_ ? " PIN" : "",
                  fi > 0 ? (std::string(" [") + flags + "]").c_str() : "");

    if (src[0])
      std::strncat(lbuf, src, sizeof(lbuf) - strlen(lbuf) - 1);
  }

  char rbuf[20];
  std::snprintf(rbuf, sizeof(rbuf), "%.0f fps ", fps);
  const int rlen = static_cast<int>(strlen(rbuf));
  const int rcol = std::max(0, COLS - rlen);

  attron(COLOR_PAIR(hudPair(2)) | A_BOLD);
  mvaddnstr(0, 0, lbuf, std::max(0, rcol - 1));
  attroff(COLOR_PAIR(hudPair(2)) | A_BOLD);

  attron(COLOR_PAIR(hudPair(3)) | A_BOLD);
  mvaddnstr(0, rcol, rbuf, std::min(rlen, COLS - rcol));
  attroff(COLOR_PAIR(hudPair(3)) | A_BOLD);

  attron(COLOR_PAIR(hudPair(0)));
  mvhline(1, 0, ACS_HLINE, COLS);
  attroff(COLOR_PAIR(hudPair(0)));
}

// ── render
// ────────────────────────────────────────────────────────────────────
void Renderer::render(const std::vector<float> &bars_l,
                      const std::vector<float> &bars_r, double fps,
                      const std::string &backend, float sens, bool auto_sens) {
  if (!initialized_ || bars_l.empty())
    return;

  const auto now_tp = Clock::now();
  if (colour_cycle_) {
    const float dt =
        std::chrono::duration<float>(now_tp - last_frame_tp_).count();
    hue_offset_ = std::fmod(hue_offset_ + HUE_DEG_PER_SEC * dt, 360.f);
    // Rebuild the palette only when the hue has moved by HUE_REBUILD_DEG.
    // Every rebuild can resend ~236 palette colours to the terminal, so at
    // 30 deg/s a 3 deg step means ~10 rebuilds/s instead of ~30 (a 3 deg shift
    // of a 38%-weighted hue blend is imperceptible).
    if (std::abs(hue_offset_ - last_rebuild_hue_) >= HUE_REBUILD_DEG) {
      last_rebuild_hue_ = hue_offset_;
      rebuildColors();
    }
  }
  last_frame_tp_ = now_tp;

  const double elapsed =
      std::chrono::duration<double>(now_tp - last_change_tp_).count();
  hud_visible_ = hud_pinned_ || (elapsed < HUD_HIDE_SECS) || feedback_active_;

  const int avail = std::max(1, LINES - HUD_ROWS);
  if (avail != prev_grad_avail_) {
    grad_steps_ = GRAD_STEPS_MAX;
    prev_grad_avail_ = avail;
    rebuildColors();
    needs_clear_ = true;
  }

  bool do_clear = needs_clear_;
  {
    const bool beat_now = nextBeatState(beat_flash_, lowBandLevel(bars_l),
                                        BEAT_THRESHOLD, BEAT_THRESHOLD_OFF);
    if (beat_now != beat_flash_) {
      do_clear = true;
      invalidatePrev();
    }
    beat_flash_ = beat_now;
  }

  if (do_clear) {
    erase();
    needs_clear_ = false;
  }

  last_bar_count_ = static_cast<int>(bars_l.size());
  drawMirrorLR(bars_l, bars_r, avail);

  if (hud_visible_)
    drawStatusBar(fps, backend, sens, auto_sens);
  else {
    mvhline(0, 0, ' ', COLS);
    mvhline(1, 0, ' ', COLS);
  }

  refresh();
}
