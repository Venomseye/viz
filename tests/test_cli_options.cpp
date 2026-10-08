// Tests for command-line parsing (pure: no printing, no exit, no config).

#include "cli_options.h"

#include <cstdio>
#include <string>
#include <vector>

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

static CliResult parse(std::vector<std::string> args) {
  std::vector<char *> argv;
  static std::string prog = "viz";
  argv.push_back(prog.data());
  for (auto &a : args)
    argv.push_back(a.data());
  argv.push_back(nullptr);
  return parseCli(static_cast<int>(argv.size()) - 1, argv.data());
}

int main() {
  { // defaults
    const auto r = parse({});
    CHECK(r.ok && r.error.empty(), "no args is fine");
    CHECK(r.opts.action == CliAction::Run, "default action is Run");
    CHECK(r.opts.backend == "auto" && r.opts.sample_rate == 44100, "defaults");
    CHECK(!r.opts.use_mic && !r.opts.auto_width, "flags default off");
    CHECK(r.opts.theme == -1 && r.opts.fps == -1,
          "theme/fps default 'not given'");
    CHECK(r.opts.color == ColorMode::Auto, "color default auto");
  }
  { // everything at once, short and long forms
    const auto r =
        parse({"-b", "pipewire", "-s", "dev.monitor", "-M", "-r", "48000", "-t",
               "6", "-f", "30", "-w", "--color", "256"});
    CHECK(r.ok, "full command line ok");
    CHECK(r.opts.backend == "pipewire", "backend");
    CHECK(r.opts.source == "dev.monitor", "source");
    CHECK(r.opts.use_mic && r.opts.auto_width, "-M and -w");
    CHECK(r.opts.sample_rate == 48000, "rate");
    CHECK(r.opts.theme == 6 && r.opts.fps == 30, "theme and fps");
    CHECK(r.opts.color == ColorMode::Palette256, "--color 256");
  }
  { // long options and --opt=value
    const auto r = parse({"--backend=pulse", "--source", "x", "--mic",
                          "--rate=96000", "--theme", "2", "--fps=120",
                          "--autowidth", "--color=truecolor"});
    CHECK(r.ok && r.opts.backend == "pulse" && r.opts.source == "x",
          "long forms");
    CHECK(r.opts.use_mic && r.opts.sample_rate == 96000, "long mic/rate");
    CHECK(r.opts.theme == 2 && r.opts.fps == 120 && r.opts.auto_width,
          "long theme/fps/width");
    CHECK(r.opts.color == ColorMode::TrueColor, "--color=truecolor");
  }
  { // actions stop parsing
    CHECK(parse({"-h"}).opts.action == CliAction::Help, "-h");
    CHECK(parse({"--help"}).opts.action == CliAction::Help, "--help");
    CHECK(parse({"-V"}).opts.action == CliAction::Version, "-V");
    CHECK(parse({"--list-sources"}).opts.action == CliAction::ListSources,
          "--list-sources");
    CHECK(parse({"--check"}).opts.action == CliAction::Check, "--check");
    // --check keeps parsing, so later options take effect:
    const auto r = parse({"--check", "--color", "256", "-t", "3"});
    CHECK(r.ok && r.opts.action == CliAction::Check, "--check parsed");
    CHECK(r.opts.color == ColorMode::Palette256 && r.opts.theme == 3,
          "options AFTER --check still apply");
    const auto l = parse({"--list-sources", "-b", "pulse"});
    CHECK(l.ok && l.opts.action == CliAction::ListSources &&
              l.opts.backend == "pulse",
          "options after --list-sources still apply");
    CHECK(!parse({"--check", "-b", "bogus"}).ok,
          "a bad option after --check is reported");
    CHECK(!parse({"-b", "bogus", "--check"}).ok,
          "a bad option before --check is reported");
    // -h / -V stop at once:
    const auto h = parse({"-h", "-b", "bogus"});
    CHECK(h.ok && h.opts.action == CliAction::Help,
          "-h wins and ignores the rest");
    CHECK(parse({"-V", "-r", "1"}).opts.action == CliAction::Version,
          "-V wins and ignores the rest");
  }
  { // errors
    auto r = parse({"-b", "alsa"});
    CHECK(!r.ok && r.error.find("unknown backend 'alsa'") != std::string::npos,
          "bad backend");
    r = parse({"-r", "100"});
    CHECK(!r.ok && r.error.find("8000 and 192000") != std::string::npos,
          "rate too low");
    r = parse({"-r", "999999"});
    CHECK(!r.ok, "rate too high");
    r = parse({"-r", "44k"});
    CHECK(!r.ok && r.error.find("expects an integer") != std::string::npos,
          "rate not integer");
    r = parse({"-t", "abc"});
    CHECK(!r.ok && r.error.find("'-t' expects an integer") != std::string::npos,
          "theme not integer");
    r = parse({"-f", "1.5"});
    CHECK(!r.ok, "fps not integer");
    r = parse({"--color", "rainbow"});
    CHECK(!r.ok && r.error.find("--color") != std::string::npos,
          "bad color mode");
    r = parse({"--bogus"});
    CHECK(!r.ok && r.show_usage, "unknown option -> usage");
    CHECK(r.error.find("--bogus") != std::string::npos,
          "unknown option is named");
    r = parse({"-s"});
    CHECK(!r.ok && r.show_usage &&
              r.error.find("requires an argument") != std::string::npos,
          "missing argument reported");
  }
  { // preserved quirks
    CHECK(parse({"-t", "-1"}).opts.theme == -1,
          "negative theme ignored, as before");
    CHECK(parse({"-f", "0"}).opts.fps == 1,
          "fps clamped up to >= 1 here (10..240 applied later)");
    CHECK(parse({"-f", "9999"}).opts.fps == 9999,
          "upper clamp happens later, not here");
  }
  { // repeat calls are independent (getopt state reset)
    const auto a = parse({"-t", "5"});
    const auto b = parse({"-f", "40"});
    CHECK(a.opts.theme == 5 && a.opts.fps == -1, "first call");
    CHECK(b.opts.theme == -1 && b.opts.fps == 40,
          "second call isn't polluted by the first");
  }
  std::printf("cli_options: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
