#pragma once
#include <cstddef>
#include <string>

struct Config {
  // ── Visual ────────────────────────────────────────────────────────────────
  int theme = 0;     // 0-11
  int bar_width = 2; // 1-8
  int gap_width = 1; // 0-2
  bool hud_pinned = false;

  // ── Rendering modes ───────────────────────────────────────────────────────
  bool colour_cycle = false;   // slowly rotate gradient hue over time
  bool per_bar_colour = false; // map colour to bar index, not screen row

  // ── Audio ─────────────────────────────────────────────────────────────────
  bool stereo = true;
  int high_cutoff = 20000; // Hz, 1000-24000

  // ── FFT / Smoothing ───────────────────────────────────────────────────────
  float gravity = 1.0f;     // fall speed multiplier (0.1-5.0)
  float monstercat = 1.5f;  // bar spread: 0=off, else 1.0-5.0 (<1 acts as 1)
  float rise_factor = 0.3f; // attack smoothing (0.0=instant, 0.95=slow)
  float bass_smooth =
      0.0f; // extra bass smoothing (0.0=off, 0.1-0.3 recommended)

  // ── Audio processing ──────────────────────────────────────────────────────
  bool a_weighting = false; // IEC 61672 perceptual frequency weighting
  float noise_gate = 0.02f; // bars below this snapped to zero (0.0-0.2)
  bool auto_mono = false;   // auto-collapse to mono on high L/R correlation

  // ── Sensitivity ───────────────────────────────────────────────────────────
  float sensitivity = 1.5f;
  bool auto_sens = true;

  // ── Performance ───────────────────────────────────────────────────────────
  int fps = 60; // 10-240

  // ── Paths ─────────────────────────────────────────────────────────────────
  static std::string configPath(); // ~/.config/viz/config
  static std::string statePath();  // ~/.local/state/viz/state

  /// Move the pre-rename ~/.config/cava-viz and ~/.local/state/cava-viz to
  /// their new "viz" names (only when the new one doesn't exist yet).
  /// Call once at startup, before load().  Returns how many were moved.
  static int migrateLegacyDirs();

  // ── Command-line overrides (-t, -f) ───────────────────────────────────────
  // These apply to the current session only.  While a flag is set, save()
  // never writes that key (the file keeps whatever it had), so
  // `viz -t 6 -f 30` followed by an unrelated key press no longer rewrites
  // the user's config.  An interactive change clears the flag.
  bool cli_theme = false;
  bool cli_fps = false;

  /// After loading a fresh Config (reload), re-apply the overrides of `prev`
  /// so the command line keeps winning over the file for this session.
  void inheritCliOverrides(const Config &prev);

  bool load();
  void
  save() const; // atomic (tmp + rename); skips the write if nothing changed

  // Digest of the bytes save() most recently wrote (0 = never saved).
  // main() compares it with currentFileDigest() on an inotify event to
  // recognise — and ignore — the event caused by its own save().
  static std::size_t lastSavedDigest();
  static std::size_t currentFileDigest(); // 0 if the config can't be read

  // Internal state — stored separately in statePath(), not configPath().
  std::string last_source;
  bool loadState();
  void saveState() const;
};
