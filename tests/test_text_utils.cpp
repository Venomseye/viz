// Tests for UTF-8 truncation, locale setup and beat-flash hysteresis.

#include "beat_detect.h"
#include "text_utils.h"

#include <clocale>
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

// True if every byte sequence in s is well-formed UTF-8 (no stray lead or
// continuation bytes).
static bool wellFormed(const std::string &s) {
  for (std::size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    std::size_t n = c < 0x80           ? 1
                    : (c >> 5) == 0x6  ? 2
                    : (c >> 4) == 0xE  ? 3
                    : (c >> 3) == 0x1E ? 4
                                       : 0;
    if (n == 0 || i + n > s.size())
      return false;
    for (std::size_t k = 1; k < n; ++k)
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80)
        return false;
    i += n;
  }
  return true;
}

int main() {
  // ── truncateUtf8 ────────────────────────────────────────────────────────
  CHECK(truncateUtf8("hello", 38) == "hello", "short string unchanged");
  CHECK(truncateUtf8("hello world", 5) == "hello", "ASCII cut exactly");
  CHECK(truncateUtf8("", 5).empty(), "empty stays empty");
  CHECK(truncateUtf8("abc", 0).empty(), "zero limit -> empty");

  const std::string cafe = "caf\xC3\xA9"; // "café": é is 2 bytes
  CHECK(truncateUtf8(cafe, 4) == "caf",
        "cut inside a 2-byte char drops it whole");
  CHECK(truncateUtf8(cafe, 5) == cafe, "limit at the end keeps it");

  const std::string jp = "\xE3\x82\xA4\xE3\x83\xB3"; // 2 x 3-byte chars
  CHECK(truncateUtf8(jp, 4) == "\xE3\x82\xA4",
        "cut inside 3-byte char backs up");
  CHECK(truncateUtf8(jp, 2).empty(), "cannot fit even one char -> empty");

  const std::string emoji = "a\xF0\x9F\x8E\xB5z"; // a + U+1F3B5 (4 bytes) + z
  for (std::size_t n = 0; n <= emoji.size() + 1; ++n)
    CHECK(wellFormed(truncateUtf8(emoji, n)),
          "every truncation of a 4-byte char stays well-formed");

  // The old behaviour (byte cut) really was broken for such input:
  CHECK(!wellFormed(cafe.substr(0, 4)),
        "byte-cutting 'café' at 4 IS malformed");

  // ── locale ──────────────────────────────────────────────────────────────
  const bool ok = initUtf8Locale();
  CHECK(ok == localeIsUtf8(), "initUtf8Locale result matches localeIsUtf8");
  CHECK(std::string(std::localeconv()->decimal_point) == ".",
        "decimal point is '.' regardless of the user's locale");
  {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", 1.5);
    CHECK(std::string(buf) == "1.50", "printf %.2f is locale-independent");
  }
  // (Whether a UTF-8 locale exists depends on the machine; just require
  //  that the two answers are consistent and nothing crashes.)

  // ── beat hysteresis ─────────────────────────────────────────────────────
  const float ON = 0.55f, OFF = 0.45f;
  CHECK(nextBeatState(false, 0.60f, ON, OFF), "turns on above ON");
  CHECK(!nextBeatState(false, 0.50f, ON, OFF), "stays off between thresholds");
  CHECK(nextBeatState(true, 0.50f, ON, OFF), "stays on between thresholds");
  CHECK(!nextBeatState(true, 0.40f, ON, OFF), "turns off below OFF");

  // A signal wobbling around 0.5 must not flicker.
  const float wobble[] = {0.60f, 0.52f, 0.56f, 0.49f,
                          0.53f, 0.47f, 0.51f, 0.46f};
  int flips_hyst = 0, flips_single = 0;
  bool h = false, s = false;
  for (float v : wobble) {
    const bool nh = nextBeatState(h, v, ON, OFF);
    const bool ns = v >= ON; // the old single-threshold rule
    flips_hyst += (nh != h);
    flips_single += (ns != s);
    h = nh;
    s = ns;
  }
  CHECK(flips_hyst == 1, "hysteresis: one clean ON, no flicker");
  CHECK(flips_single >= 4, "single threshold flickers on the same input");

  CHECK(lowBandLevel({}) == 0.f, "empty bars -> 0");
  CHECK(lowBandLevel({0.4f, 0.8f}) > 0.59f &&
            lowBandLevel({0.4f, 0.8f}) < 0.61f,
        "mean of fewer than n bars");
  CHECK(lowBandLevel({1.f, 1.f, 1.f, 1.f, 0.f, 0.f}) == 1.f,
        "only the lowest 4 bars count");

  std::printf("text_utils: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
