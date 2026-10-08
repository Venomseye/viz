#include "cli_options.h"

#include "config.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <getopt.h>
#include <stdexcept>

namespace {

// Integer parser that reports instead of throwing/exiting.
bool parseInt(const char *s, int &out) {
  try {
    std::size_t pos = 0;
    const int v = std::stoi(s, &pos);
    if (pos != std::strlen(s))
      return false;
    out = v;
    return true;
  } catch (...) {
    return false;
  }
}

CliResult fail(const std::string &msg) {
  CliResult r;
  r.ok = false;
  r.error = msg;
  return r;
}

} // namespace

CliResult parseCli(int argc, char *argv[]) {
  CliResult res;
  CliOptions &o = res.opts;

  enum { OPT_LIST_SOURCES = 1000, OPT_CHECK, OPT_COLOR };
  static const struct option long_opts[] = {
      {"backend", required_argument, nullptr, 'b'},
      {"source", required_argument, nullptr, 's'},
      {"mic", no_argument, nullptr, 'M'},
      {"rate", required_argument, nullptr, 'r'},
      {"theme", required_argument, nullptr, 't'},
      {"fps", required_argument, nullptr, 'f'},
      {"autowidth", no_argument, nullptr, 'w'},
      {"color", required_argument, nullptr, OPT_COLOR},
      {"version", no_argument, nullptr, 'V'},
      {"help", no_argument, nullptr, 'h'},
      {"list-sources", no_argument, nullptr, OPT_LIST_SOURCES},
      {"check", no_argument, nullptr, OPT_CHECK},
      {nullptr, 0, nullptr, 0}};

  optind = 0; // glibc: full re-initialisation, so parseCli() can be called
              // repeatedly
  opterr = 0; // we report errors ourselves
  int c;
  while ((c = getopt_long(argc, argv, "b:s:Mr:t:f:wVh", long_opts, nullptr)) !=
         -1) {
    switch (c) {
    case 'b':
      o.backend = optarg;
      if (o.backend != "auto" && o.backend != "pipewire" &&
          o.backend != "pulse")
        return fail("viz: unknown backend '" + o.backend +
                    "' (use auto, pipewire or pulse)");
      break;
    case 's':
      o.source = optarg;
      break;
    case 'M':
      o.use_mic = true;
      break;
    case 'r': {
      int v = 0;
      if (!parseInt(optarg, v))
        return fail(std::string("viz: '-r' expects an integer, got '") +
                    optarg + "'");
      if (v < 8000 || v > 192000)
        return fail("viz: -r must be between 8000 and 192000 Hz, got " +
                    std::to_string(v));
      o.sample_rate = v;
      break;
    }
    case 't': {
      int v = 0;
      if (!parseInt(optarg, v))
        return fail(std::string("viz: '-t' expects an integer, got '") +
                    optarg + "'");
      if (v >= 0) // negative = ignored, as before
        o.theme = v;
      break;
    }
    case 'f': {
      int v = 0;
      if (!parseInt(optarg, v))
        return fail(std::string("viz: '-f' expects an integer, got '") +
                    optarg + "'");
      o.fps = std::max(1, v);
      break;
    }
    case 'w':
      o.auto_width = true;
      break;
    case OPT_COLOR:
      if (!parseColorMode(optarg, o.color))
        return fail(std::string("viz: unknown --color mode '") + optarg +
                    "' (use auto, truecolor, 256 or basic)");
      break;
    case 'V':
      o.action = CliAction::Version; // prints and exits: nothing else matters
      return res;
    case 'h':
      o.action = CliAction::Help;
      return res;
    // --list-sources / --check report on the EFFECTIVE settings, so keep
    // parsing: `viz --check --color 256 -t 3` must honour both options.
    case OPT_LIST_SOURCES:
      o.action = CliAction::ListSources;
      break;
    case OPT_CHECK:
      o.action = CliAction::Check;
      break;
    case '?':
    default: {
      CliResult r = fail("");
      r.show_usage = true;
      // getopt stored the offending option; mirror its usual wording.
      if (optopt != 0 && std::strchr("bsrtf", optopt))
        r.error = std::string("viz: option '-") + static_cast<char>(optopt) +
                  "' requires an argument";
      else if (optind > 0 && optind <= argc && argv[optind - 1] &&
               argv[optind - 1][0] == '-')
        r.error =
            std::string("viz: unrecognised option '") + argv[optind - 1] + "'";
      return r;
    }
    }
  }
  return res;
}

void printUsage(const char *p) {
  std::printf(
      "Usage: %s [OPTIONS]\n\n"
      "Terminal audio visualizer (CAVA algorithm)\n\n"
      "Options:\n"
      "  -b <pulse|pipewire|auto>   Backend (default: auto)\n"
      "  -s <source>                Explicit source device\n"
      "  -M                         Use microphone input\n"
      "  -r <Hz>                    Sample rate (default: 44100)\n"
      "  -t <index>                 Theme index (0-11 built-in, 12+ user)\n"
      "  -f <n>                     Target FPS (default: 60)\n"
      "  -w                         Auto bar width\n"
      "  --color <mode>             auto | truecolor | 256 | basic\n"
      "  --list-sources             List available audio sources and exit\n"
      "  --check                    Validate config and exit\n"
      "  -V                         Show version and exit\n"
      "  -h                         Show help\n\n"
      "Keys:\n"
      "  q          Quit\n"
      "  t          Next theme\n"
      "  g          Cycle gap (0-2)\n"
      "  ] / [      Increase / decrease bar width\n"
      "  UP / DOWN  Manual sensitivity\n"
      "  a          Toggle auto-sensitivity\n"
      "  s          Toggle stereo / mono\n"
      "  h          Toggle HUD pin\n"
      "  c          Toggle colour cycle\n"
      "  v          Toggle per-bar colour\n"
      "  w          Toggle A-weighting\n"
      "  n          Toggle auto-mono\n\n"
      "Live reload:\n"
      "  pkill -USR1 -x viz         Reload config + themes without "
      "restarting\n\n"
      "Config: %s\n"
      "  Edit while running — inotify reloads changes instantly.\n\n"
      "Themes: Fire Plasma Neon Teal Sunset Candy Aurora Inferno "
      "White Rose Mermaid Vapor\n",
      p, Config::configPath().c_str());
}
