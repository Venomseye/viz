// Tests for the bounded subprocess helper and monitor polling.
// No sound server needed: detectMonitor() just has to return promptly.

#include "audio_utils.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <sys/wait.h>

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

using Clock = std::chrono::steady_clock;
static long msSince(Clock::time_point t) {
  return static_cast<long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t)
          .count());
}

int main() {
  // Normal output, passed through without a shell.
  CHECK(runProcess({"printf", "hello"}) == "hello", "captures stdout");
  CHECK(runProcess({"sh", "-c", "printf 'a b'; echo oops >&2"}) == "a b",
        "stderr is discarded");

  // Failure modes all give "".
  CHECK(runProcess({"this-binary-does-not-exist-xyz"}).empty(),
        "missing binary -> empty");
  CHECK(runProcess({"sh", "-c", "printf data; exit 3"}).empty(),
        "non-zero exit -> empty");
  CHECK(runProcess({}).empty(), "empty argv -> empty");

  // A hung command must not block: killed at the deadline and reaped.
  {
    const auto t0 = Clock::now();
    const std::string r = runProcess({"sleep", "10"}, 300);
    const long ms = msSince(t0);
    CHECK(r.empty(), "timeout -> empty");
    CHECK(ms >= 250 && ms < 2000, "returns close to the deadline, not 10 s");
  }
  {
    const auto t0 = Clock::now();
    const std::string r =
        runProcess({"sh", "-c", "printf partial; sleep 10"}, 300);
    CHECK(r.empty(), "timeout discards partial output");
    CHECK(msSince(t0) < 2000, "second hang also returns promptly");
  }

  // Closes stdout but keeps running: still must not hang.
  {
    const auto t0 = Clock::now();
    (void)runProcess({"sh", "-c", "exec >&-; sleep 10"}, 2000);
    CHECK(msSince(t0) < 3000, "child that closes stdout but lingers is killed");
  }

  // Large output is capped, not unbounded.
  {
    const std::string r =
        runProcess({"sh", "-c", "head -c 300000 /dev/zero | tr '\\0' x"}, 3000);
    CHECK(r.size() <= (1u << 16) + 4096, "output is capped");
  }

  // No zombies left behind by any of the above.
  errno = 0;
  CHECK(waitpid(-1, nullptr, WNOHANG) == -1 && errno == ECHILD,
        "no unreaped children");

  // HUD labels: the mic sentinel is never shown raw.
  CHECK(sourceLabel(AudioCapture::MIC_SOURCE) == "default microphone",
        "mic sentinel gets a human label");
  CHECK(sourceLabel("alsa_output.pci.monitor") == "alsa_output.pci.monitor",
        "real source names pass through");
  CHECK(sourceLabel("").empty(), "empty source stays empty");

  // detectMonitor(): first call bounded, later calls instant (cached).
  {
    const auto t0 = Clock::now();
    const std::string a = detectMonitor();
    CHECK(msSince(t0) < 4000, "first detectMonitor() is bounded");
    const auto t1 = Clock::now();
    for (int i = 0; i < 1000; ++i)
      (void)detectMonitor();
    CHECK(msSince(t1) < 500, "cached detectMonitor() never blocks");
    CHECK(detectMonitor() == a, "value stable between polls");
  }

  std::printf("audio_utils: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
