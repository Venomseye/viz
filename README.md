# viz

[![CI](https://github.com/Venomseye/viz/actions/workflows/ci.yml/badge.svg)](https://github.com/Venomseye/viz/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![Platform: Linux](https://img.shields.io/badge/platform-Linux-lightgrey.svg)

A real-time terminal audio visualizer written in C++17. It implements the [CAVA](https://github.com/karlstav/cava) analysis pipeline (dual FFT, Monstercat smoothing, per-bar EQ, auto-sensitivity) and draws it with ncurses using smooth truecolor gradients, mirrored left/right channels and live-reloadable themes.

```text
 PipeWire  Neon  W:2 G:1  Sens:1.5 A PIN  alsa_output.pci-0000_00_1f.3.monit 60 fps
────────────────────────────────────────────────────────────────────────────────────

                                    ▅▅       ▅▅
                                    ██       ██
                                    ██       ██
                                    ██       ██
                                    ██ ██ ██ ██
                                 ▂▂ ██ ██ ██ ██ ▂▂
                                 ██ ██ ██ ██ ██ ██
                                 ██ ██ ██ ██ ██ ██    ▁▁
▆▆ ▆▆ ▅▅ ▃▃ ▂▂ ▁▁    ▃▃ ▅▅ ▄▄ ▇▇ ██ ██ ██ ██ ██ ██ ▇▇ ██    ▃▃ ▃▃       ▁▁    ▁▁ ▂▂
██ ██ ██ ██ ██ ██ ▆▆ ██ ██ ██ ██ ██ ██ ██ ██ ██ ██ ██ ██ ▇▇ ██ ██ ▅▅ ▅▅ ██ ██ ██ ██
```

<sub>A frame from the real renderer, captured as plain text (colours omitted) while analysing a synthetic stereo test signal: bass at the centre, highs toward the edges.</sub>

## Features

- **CAVA-style analysis** &mdash; dual FFT (separate bass window), log-spaced bars, per-bar EQ, gravity/integral smoothing, Monstercat spreading, auto-sensitivity.
- **Stereo view** &mdash; left channel on the left half, right on the right, mirrored around the centre; collapses to mono automatically when the channels are nearly identical (optional).
- **12 built-in themes + your own** &mdash; drop a `.theme` file with 2&ndash;8 colour stops into the config folder; it loads instantly, no restart.
- **Colour that adapts to your terminal** &mdash; truecolor where available, nearest-match 256-colour or 8-colour otherwise.
- **Live reload** &mdash; edit the config or themes while it runs (inotify), or send `SIGUSR1`.
- **PipeWire and PulseAudio** &mdash; auto-detects the backend and the default output's monitor, follows default-device changes, and reconnects if the stream drops.
- **Efficient** &mdash; absolute-deadline frame pacing (`clock_nanosleep`, no busy-wait) and delta drawing: only changed cells are redrawn.

## Quick start

```bash
git clone https://github.com/Venomseye/viz.git
cd viz
./install.sh        # installs dependencies, builds, installs to /usr/local
viz                 # play some music
```

Press `t` to change theme, `q` to quit. The full key list is [below](#controls).

## Requirements

Linux, a terminal with Unicode block characters (truecolor recommended), and at least one audio backend.

| Dependency | Arch | Debian / Ubuntu |
|---|---|---|
| C++17 compiler | `gcc` or `clang` | `g++` or `clang++` |
| CMake &ge; 3.16 | `cmake` | `cmake` |
| Ninja *(optional, faster)* | `ninja` | `ninja-build` |
| FFTW3 | `fftw` | `libfftw3-dev` |
| ncursesw | `ncurses` | `libncursesw5-dev` |
| PipeWire *(backend)* | `pipewire` | `libpipewire-0.3-dev` `libspa-0.2-dev` |
| PulseAudio *(backend)* | `libpulse` | `libpulse-dev` |

At least one of PipeWire / PulseAudio must be present at build time. `pactl` (from `libpulse` / `pulseaudio-utils`; also provided by `pipewire-pulse`) is used to find your default output and to list sources.

## Installation

### Prebuilt release

Download the tarball from the [Releases](https://github.com/Venomseye/viz/releases) page, then:

```bash
tar xzf viz-v*-linux-x86_64.tar.gz && cd viz-v*-linux-x86_64
./install.sh                            # to /usr/local (uses sudo if needed)
INSTALL_PREFIX=~/.local ./install.sh    # or to your home directory, no sudo
```

This installs the binary, man page, shell completions and an example theme. You still need the runtime libraries (`fftw`, `ncurses`, and PipeWire and/or PulseAudio).

### Build from source

```bash
git clone https://github.com/Venomseye/viz.git
cd viz
./install.sh
```

`install.sh` detects your distribution, installs missing dependencies, builds with CMake (Ninja if available) and installs the binary, man page and completions.

```bash
./install.sh --clean                    # wipe the build directory first
./install.sh --test                     # run the unit tests before installing
./install.sh --skip-deps                # skip the dependency check
INSTALL_PREFIX=~/.local ./install.sh    # custom prefix
```

Or do it by hand:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
sudo cmake --install build
```

Builds are portable by default. `-DNATIVE_ARCH=ON` adds `-march=native` for a binary tuned to *this* machine only; never use it for packages or releases.

### Arch Linux

A `PKGBUILD` is included. It builds from the tagged release (`v1.2.0`), so replace the `SKIP` checksum with the real one after tagging:

```bash
makepkg -si
```

### Uninstall

```bash
./uninstall.sh          # asks before removing your config
./uninstall.sh --yes    # removes everything without asking
```

## Usage

```text
viz [OPTIONS]
```

| Option | Description | Default |
|---|---|---|
| `-b <auto\|pipewire\|pulse>` | Audio backend | `auto` |
| `-s <source>` | Capture a specific source; reconnects to it if it drops | the default output's monitor |
| `-M` | Capture the default **microphone** instead of the system audio | off |
| `-r <Hz>` | Sample rate (8000&ndash;192000) | `44100` |
| `-t <index>` | Starting theme (0&ndash;11 built-in, 12+ user). Session only, not saved | from config |
| `-f <n>` | Target FPS. Session only, not saved | from config |
| `-w` | Fit the bar width to the terminal | off |
| `--color <mode>` | Colour strategy: `auto`, `truecolor`, `256` or `basic` (see [Terminal support](#terminal-support)) | `auto` |
| `--list-sources` | List audio sources and exit | |
| `--check` | Validate config, themes, audio and terminal setup, then exit. Options after it (e.g. `--color 256`) are honoured, so you can preview their effect | |
| `-V` | Print the version and exit | |
| `-h` | Print help and exit | |

Long forms (`--backend`, `--source`, `--mic`, `--rate`, `--theme`, `--fps`, `--autowidth`) are accepted too.

```bash
viz                                          # auto-detect everything
viz -M                                       # visualise the microphone
viz -b pipewire -s alsa_output.pci-0000_00_1f.3.monitor
viz -t 6 -f 30                               # Aurora theme at 30 fps, just this run
viz --list-sources                           # what can I capture?
viz --check                                  # is my setup OK?
```

By default viz visualises **what you are listening to** (the monitor of your default output). It switches automatically if you change the default output device while it runs. With `-s` or `-M` your choice is never overridden; if that stream dies, the same source is re-opened (retrying at growing intervals, at most every 10 s).

## Controls

| Key | Action |
|---|---|
| `q` | Quit |
| `t` | Next theme (built-in, then your own, then wrap) |
| `g` | Cycle the gap between bars: 0 &rarr; 1 &rarr; 2 |
| `]` / `[` | Wider / narrower bars |
| `↑` / `↓` | Manual sensitivity |
| `a` | Toggle auto-sensitivity |
| `s` | Toggle stereo / mono |
| `h` | Pin the status bar (otherwise it hides after 3 s of inactivity) |
| `c` | Colour cycle: slowly rotate the gradient hue |
| `v` | Per-bar colour: colour by bar position instead of height |
| `w` | A-weighting: perceptual (IEC 61672) frequency weighting |
| `n` | Auto-mono: collapse to mono when left and right match |

Changes you make with these keys are saved to the config file immediately.

The status bar shows the backend, theme, bar width (`W`) and gap (`G`), sensitivity (`A` = auto), `PIN`, active modes (`C` colour cycle, `B` per-bar colour), the capture source and the frame rate.

## Configuration

The config file is created on first run at `${XDG_CONFIG_HOME:-~/.config}/viz/config`. Edit it while viz is running and the changes apply within a moment. Out-of-range values are clamped.

| Key | Default | Range | What it does |
|---|---|---|---|
| `theme` | `0` | 0&ndash;11, 12+ = user | Colour theme |
| `bar_width` | `2` | 1&ndash;8 | Bar width in columns |
| `gap_width` | `1` | 0&ndash;2 | Gap between bars |
| `hud_pinned` | `0` | 0 / 1 | Always show the status bar |
| `colour_cycle` | `0` | 0 / 1 | Slowly rotate the gradient hue |
| `per_bar_colour` | `0` | 0 / 1 | Colour by bar position |
| `stereo` | `1` | 0 / 1 | Stereo (1) or mono (0) |
| `high_cutoff` | `20000` | 1000&ndash;24000 Hz | Highest frequency shown |
| `gravity` | `1.00` | 0.1&ndash;5.0 | Fall speed (higher is faster) |
| `monstercat` | `1.50` | 0 = off, 1.0&ndash;5.0 | Bar spreading; a **higher** value spreads **less**; values between 0 and 1 act as 1.0 |
| `rise_factor` | `0.30` | 0.0&ndash;0.95 | Attack smoothing (0 = instant) |
| `bass_smooth` | `0.00` | 0.0&ndash;1.0 | Extra smoothing for bass bars (0.1&ndash;0.3 is typical) |
| `a_weighting` | `0` | 0 / 1 | Perceptual frequency weighting |
| `noise_gate` | `0.020` | 0.0&ndash;0.2 | Bars below this snap to zero |
| `auto_mono` | `0` | 0 / 1 | Collapse to mono when L and R are nearly identical |
| `sensitivity` | `1.50` | 0.2&ndash;8.0 | Manual sensitivity |
| `auto_sens` | `1` | 0 / 1 | Automatic sensitivity |
| `fps` | `60` | 10&ndash;240 | Target frame rate (applies live) |

<details>
<summary>The file as generated on first run</summary>

```ini
# viz configuration
# Edit while running — inotify reloads changes instantly.

# ── Visual ──────────────────────────────────────────────────────
# 0=Fire 1=Plasma 2=Neon 3=Teal 4=Sunset 5=Candy
# 6=Aurora 7=Inferno 8=White 9=Rose 10=Mermaid 11=Vapor
theme          = 0
bar_width      = 2
gap_width      = 1
hud_pinned     = 0

# ── Rendering modes ──────────────────────────────────────────────
# colour_cycle: slowly rotate gradient hue over time
colour_cycle   = 0
# per_bar_colour: map colour to bar index (bass=base, treble=tip)
per_bar_colour = 0

# ── Audio ────────────────────────────────────────────────────────
stereo         = 1
high_cutoff    = 20000

# ── FFT / Smoothing ──────────────────────────────────────────────
# gravity: fall speed (0.1=slow, 1.0=default, 5.0=instant)
gravity        = 1.00
# monstercat: bar spread (0=off, 1.0-5.0; values in (0,1) act as 1.0; 1.5=default)
monstercat     = 1.50
# rise_factor: attack smoothing (0.0=instant, 0.95=very slow)
rise_factor    = 0.30
# bass_smooth: extra smoothing for bass bars (0.0=off, 0.1-0.3 recommended)
bass_smooth    = 0.00

# ── Audio processing ─────────────────────────────────────────────
# a_weighting: IEC 61672 perceptual frequency weighting
a_weighting    = 0
# noise_gate: bars below this (post-sens) snap to zero (0.0-0.2)
noise_gate     = 0.020
# auto_mono: collapse stereo to mono when L/R are nearly identical
auto_mono      = 0

# ── Sensitivity ──────────────────────────────────────────────────
sensitivity    = 1.50
auto_sens      = 1

# ── Performance ──────────────────────────────────────────────────
fps            = 60
```

</details>

`-t` and `-f` on the command line apply to that run only and are never written to the file. Your own comments in the config are preserved when viz saves.

### Reloading

Edits to the config and to files in the themes folder are picked up automatically. You can also trigger it by hand, which is handy over SSH or where inotify isn't available:

```bash
pkill -USR1 -x viz
```

If you change `stereo`, the audio capture restarts to match; `fps` takes effect immediately.

## Themes

### Built-in

| # | Name | Gradient |
|---|---|---|
| 0 | Fire | Deep red &rarr; amber &rarr; pale yellow |
| 1 | Plasma | Magenta &rarr; violet &rarr; electric blue |
| 2 | Neon | Cyan &rarr; electric green |
| 3 | Teal | Deep teal &rarr; sky blue &rarr; white |
| 4 | Sunset | Deep purple &rarr; salmon &rarr; gold |
| 5 | Candy | Hot pink &rarr; lavender &rarr; mint |
| 6 | Aurora | Deep navy &rarr; emerald &rarr; cyan |
| 7 | Inferno | Black &rarr; deep red &rarr; bright orange |
| 8 | White | Cool grey &rarr; pure white |
| 9 | Rose | Dark maroon &rarr; rose &rarr; blush |
| 10 | Mermaid | Deep indigo &rarr; teal &rarr; seafoam |
| 11 | Vapor | Deep purple &rarr; pink &rarr; pale cyan |

### Your own

Put `.theme` files in `${XDG_CONFIG_HOME:-~/.config}/viz/themes/`. They are numbered from 12 in alphabetical order of file name and cycle with `t`. Adding, editing or removing a file takes effect immediately.

```ini
# ~/.config/viz/themes/ocean.theme
name   = Ocean

stop_0 = 0.00  #003366
stop_1 = 0.40  #0055aa
stop_2 = 0.75  #00aaee
stop_3 = 1.00  #00ffcc
```

Each `stop_N` is a position from 0.0 (bottom of a bar) to 1.0 (top) and a `#RRGGBB` colour. Use 2&ndash;8 stops, in any order; `name` is optional and `#` starts a comment. `viz --check` lists the themes it loaded. An example is in [`examples/ocean.theme`](examples/ocean.theme).

## Terminal support

- **Truecolor** is detected from `COLORTERM`, and from Konsole, Kitty, VTE (GNOME Terminal and friends), Windows Terminal and iTerm2. Elsewhere viz falls back to the nearest colours of the 256-colour palette, or to the 8 basic colours. On detection viz may switch `TERM` to `xterm-direct`, but only if that terminfo entry exists on your machine (otherwise ncurses could not start at all).
- **Override it** with `--color`: `truecolor` forces the exact-RGB path, `256` never redefines colours and uses the fixed xterm palette (the safe choice if colours look wrong or your terminal misbehaves), `basic` uses the 8 ANSI colours.
- **See what was decided** with `viz --check`: its *Terminal* section shows `TERM`, `COLORTERM`, whether truecolor was detected, whether `xterm-direct` is installed, the chosen `TERM` override and the locale.
- **UTF-8** is required for the bar glyphs (`▁▂▃▄▅▆▇█`). If your locale isn't UTF-8 (typical over SSH or in containers) viz switches to `C.UTF-8` automatically.
- Use a font that includes the Unicode *Block Elements* range; most monospace fonts do.

## How it works

1. **Capture**: a PipeWire or PulseAudio thread hands raw samples to the analyser through a lock-free ring buffer; the audio thread never blocks or allocates.
2. **Analyse**: two FFTs run per frame (a longer window for bass below 100 Hz, a shorter one for the rest). Bins are grouped into log-spaced bars between 50 Hz and `high_cutoff`, then weighted by the CAVA per-bar EQ.
3. **Smooth**: gravity and integral memory, optional rise smoothing, Monstercat spreading, a noise gate and auto-sensitivity that backs off when bars overshoot.
4. **Draw**: ncurses renders bars with eighth-block glyphs for sub-cell height, left channel on the left half and right on the right, mirrored so bass is in the middle (in mono both halves show the same signal).

## Signals

| Signal | Effect |
|---|---|
| `SIGUSR1` | Reload config and themes |
| `SIGINT`, `SIGTERM`, `SIGHUP` | Quit cleanly (`SIGHUP` is what a closing terminal sends) |
| `SIGWINCH` | Terminal resized (handled automatically) |

## Files

Follows the [XDG Base Directory spec](https://specifications.freedesktop.org/basedir-spec/latest/):

| Path | Purpose |
|---|---|
| `${XDG_CONFIG_HOME:-~/.config}/viz/config` | Configuration |
| `${XDG_CONFIG_HOME:-~/.config}/viz/themes/` | Your `.theme` files |
| `${XDG_STATE_HOME:-~/.local/state}/viz/state` | Last-used audio source |

Settings from the project's earlier name (`cava-viz/` in those two locations) are moved to `viz/` automatically on first launch.

## Troubleshooting

Start with `viz --check`: it prints the resolved paths, your settings, the detected monitor and the themes it loaded.

**The bars don't move**

```bash
viz --list-sources                                # is anything listed?
viz -s "$(pactl get-default-sink).monitor"        # force the default monitor
systemctl --user status pipewire                  # is the server running?
```

Make sure audio is actually playing through the *default* output.

**It shows my microphone / the wrong device**

Without `-M` viz follows the default output's monitor. Use `viz --list-sources` and pick one with `-s`.

**Everything is a flat line of `▁`**

The source is silent. Check `pactl get-default-sink` and `pactl list short sources | grep monitor`.

**Boxes or garbled characters instead of bars**

Use a font with Unicode block characters and make sure your terminal is set to UTF-8. Over SSH or in a container try `LANG=C.UTF-8 viz`.

**The gradient looks banded, wrong or has only a few colours**

Run `viz --check` and read its *Terminal* section. Your terminal may not support truecolor: check that `echo $COLORTERM` prints `truecolor` or `24bit`. Kitty, WezTerm, Alacritty, Konsole, GNOME Terminal and most modern terminals do. If it still looks wrong, force the portable mode: `viz --color 256`.

**viz refuses to start with "Error opening terminal"**

Your `TERM` has no terminfo entry on this machine. Set a common one (`TERM=xterm-256color viz`) or install `ncurses-term`.

**`pkill -USR1 -x viz` does nothing**

The process name is `viz`; check with `pgrep -x viz`.

## Development

```bash
./install.sh --test                      # build and run the tests
cd build && ctest --output-on-failure    # or run them directly

scripts/format.sh                        # format everything (clang-format-18)
scripts/format.sh --check                # dry run; non-zero if anything would change
clang-tidy -p build src/*.cpp            # lint
```

The test suites (`ctest`) cover the config parser, user themes, the FFT pipeline, audio helpers, text utilities, terminal colour decisions, the command line, the reconnect watchdog, and the file watcher and frame pacing. CI (`.github/workflows/ci.yml`) runs a formatting check, clang-tidy and a build plus tests in Release and in Debug with AddressSanitizer/UBSan; another workflow builds with each audio backend on its own.

If the **Format** check fails, run *Actions &rarr; Format code &rarr; Run workflow*: it formats the branch with the same clang-format as CI and commits the result.

```text
src/
  main.cpp               signals, --check, and the main loop
  session.*              runtime state: audio, FFT, renderer, keys, reloads
  cli_options.*          command-line parsing (pure, unit-tested)
  watchdog.*             when to reconnect the audio (pure state machine)
  config_watcher.*       inotify watch of the config and themes folder
  frame_limiter.*        absolute-deadline frame pacing
  fft_processor.*        CAVA analysis: FFT, EQ, smoothing, auto-sensitivity
  renderer.*             ncurses drawing, themes, palette handling
  terminal_caps.*        colour-mode detection, safe TERM override (pure)
  config.*               config + state files, atomic saves, migration
  user_theme.*           .theme parser
  audio_capture.h        capture interface
  pipewire_capture.*     PipeWire backend
  pulse_capture.*        PulseAudio backend
  audio_utils.*          backend selection, source discovery
  text_utils.*           UTF-8 helpers, locale setup
  beat_detect.h          beat-flash hysteresis
tests/                   unit tests (run by ctest)
scripts/                 format.sh, install-prebuilt.sh
completions/ man/ examples/
```

## Credits and license

viz is a C++ port of the analysis algorithm from [CAVA](https://github.com/karlstav/cava) by Karl Stavestrand (MIT).

viz itself is released under the [MIT license](LICENSE). It links against [FFTW](https://www.fftw.org/), which is licensed under the GPL, so a binary you distribute must also satisfy FFTW's license terms.
