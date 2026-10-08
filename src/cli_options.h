#pragma once
#include "terminal_caps.h"

#include <string>

/// What the command line asked viz to do.
enum class CliAction { Run, Help, Version, ListSources, Check };

struct CliOptions {
  CliAction action = CliAction::Run;
  std::string backend = "auto";      // auto | pipewire | pulse
  std::string source;                // -s
  int sample_rate = 44100;           // -r
  bool use_mic = false;              // -M
  bool auto_width = false;           // -w
  int theme = -1;                    // -t, -1 = not given
  int fps = -1;                      // -f, -1 = not given
  ColorMode color = ColorMode::Auto; // --color
};

struct CliResult {
  CliOptions opts;
  bool ok = true;          // false: print `error` (if any) and exit 1
  bool show_usage = false; // with ok == false: also print the usage text
  std::string error;       // ready-to-print message, "" if none
};

/// Parse argv without side effects: no printing, no exit(), no config access.
/// -h and -V stop parsing at once (they print and exit).  --list-sources and
/// --check keep parsing so the options after them still apply.  The first
/// invalid option is always reported, wherever it appears before a stop.
CliResult parseCli(int argc, char *argv[]);

void printUsage(const char *prog);
