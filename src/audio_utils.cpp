#include "audio_utils.h"

#ifdef HAVE_PULSEAUDIO
#include "pulse_capture.h"
#endif
#ifdef HAVE_PIPEWIRE
#include "pipewire_capture.h"
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char **environ;

// ── Bounded subprocess execution ─────────────────────────────────────────────
//
// Replaces popen("pactl ... | awk ..."): no shell, stderr discarded, and a hard
// deadline.  A hung PulseAudio/PipeWire server used to block popen()/pclose()
// forever on the render thread and freeze the whole UI.

std::string runProcess(const std::vector<std::string> &args, int timeout_ms) {
  using Clock = std::chrono::steady_clock;
  if (args.empty())
    return "";

  int pfd[2];
  if (pipe2(pfd, O_CLOEXEC) != 0)
    return "";

  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&fa, pfd[1], STDOUT_FILENO);
  posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY,
                                   0);

  // Own process group, so a timeout can kill wrapper scripts AND whatever
  // they spawned (kill(-pid)), not just the direct child.
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
  posix_spawnattr_setpgroup(&attr, 0);

  std::vector<char *> argv;
  argv.reserve(args.size() + 1);
  for (const auto &a : args)
    argv.push_back(const_cast<char *>(a.c_str()));
  argv.push_back(nullptr);

  pid_t pid = 0;
  const int rc = posix_spawnp(&pid, argv[0], &fa, &attr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&attr);
  close(pfd[1]);
  if (rc != 0) { // e.g. ENOENT: pactl not installed — fails instantly
    close(pfd[0]);
    return "";
  }

  static constexpr std::size_t MAX_OUT = 1 << 16;
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  std::string out;
  bool timed_out = false;
  char buf[4096];
  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                          deadline - Clock::now())
                          .count();
    if (left <= 0) {
      timed_out = true;
      break;
    }
    pollfd pf{pfd[0], POLLIN, 0};
    const int pr = poll(&pf, 1, static_cast<int>(left));
    if (pr < 0) {
      if (errno == EINTR)
        continue;
      timed_out = true; // treat hard poll errors like a failure
      break;
    }
    if (pr == 0) {
      timed_out = true;
      break;
    }
    const ssize_t n = read(pfd[0], buf, sizeof(buf));
    if (n > 0) {
      if (out.size() < MAX_OUT)
        out.append(buf, static_cast<std::size_t>(n));
    } else if (n == 0) {
      break; // EOF
    } else if (errno != EINTR && errno != EAGAIN) {
      timed_out = true;
      break;
    }
  }
  close(pfd[0]);

  // Reap the child.  If it closed stdout but lingers (or we timed out), give
  // it a moment and then SIGKILL so we never leave a zombie or block.
  int status = 0;
  bool reaped = false;
  if (!timed_out) {
    for (int i = 0; i < 20 && !reaped; ++i) {
      const pid_t w = waitpid(pid, &status, WNOHANG);
      if (w == pid || (w < 0 && errno != EINTR))
        reaped = true;
      else
        usleep(10'000);
    }
  }
  if (!reaped) {
    kill(-pid, SIGKILL); // whole group (see above)
    kill(pid, SIGKILL);  // in case setpgid raced
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
  }
  if (timed_out || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    return "";
  return out;
}

static std::vector<std::string> splitLines(const std::string &s) {
  std::vector<std::string> lines;
  std::size_t pos = 0;
  while (pos < s.size()) {
    std::size_t nl = s.find('\n', pos);
    if (nl == std::string::npos)
      nl = s.size();
    std::string line = s.substr(pos, nl - pos);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
      line.pop_back();
    lines.push_back(std::move(line));
    pos = nl + 1;
  }
  return lines;
}

// `pactl list short sources` → names (2nd tab-separated column).
static std::vector<std::string> parseSourceNames(const std::string &out) {
  std::vector<std::string> names;
  for (const auto &line : splitLines(out)) {
    // pactl format: id<TAB>name<TAB>module<TAB>format<TAB>state
    const std::size_t t1 = line.find('\t');
    if (t1 == std::string::npos)
      continue;
    const std::size_t t2 = line.find('\t', t1 + 1);
    std::string name = line.substr(
        t1 + 1, t2 == std::string::npos ? std::string::npos : t2 - t1 - 1);
    if (!name.empty())
      names.push_back(std::move(name));
  }
  return names;
}

// ── Source enumeration
// ────────────────────────────────────────────────────────

std::vector<std::string> listSources() {
  return parseSourceNames(
      runProcess({"pactl", "list", "short", "sources"}, 2000));
}

void printSources(const Config &cfg) {
  const std::string monitor = detectMonitor();
  const auto sources = listSources();

  if (sources.empty()) {
    printf("No sources found. Is PipeWire or PulseAudio running?\n");
    printf("Try: pactl list short sources\n");
    return;
  }

  printf("Available audio sources:\n\n");
  for (const auto &s : sources) {
    const bool is_monitor = (s == monitor);
    const bool is_last = (s == cfg.last_source);
    printf("  %-60s", s.c_str());
    if (is_monitor)
      printf("  ← default monitor");
    if (is_last)
      printf("  ← last used");
    printf("\n");
  }
  printf("\nPass a source with:  viz -s \"<name>\"\n");
  printf("Monitor shorthand:   viz  (auto-detects the default monitor)\n");
  printf("Microphone:          viz -M\n");
}

// ── Monitor detection
// ─────────────────────────────────────────────────────────

static constexpr int PACTL_TIMEOUT_MS = 1000;
static constexpr int MONITOR_POLL_SECS = 5;

std::string queryDefaultMonitor() {
  // Method 1: get-default-sink (fast; PulseAudio and pipewire-pulse).
  std::string sink;
  {
    const auto lines =
        splitLines(runProcess({"pactl", "get-default-sink"}, PACTL_TIMEOUT_MS));
    if (!lines.empty())
      sink = lines.front();
  }
  if (!sink.empty())
    return sink + ".monitor";

  // Method 2: first *.monitor in the source list.
  const auto names = parseSourceNames(
      runProcess({"pactl", "list", "short", "sources"}, PACTL_TIMEOUT_MS));
  for (const auto &n : names)
    if (n.find(".monitor") != std::string::npos)
      return n;
  return "";
}

namespace {
// Keeps the default-monitor name fresh on a background thread so the render
// loop never waits on pactl.  The first call is synchronous (startup, before
// ncurses, bounded by PACTL_TIMEOUT_MS per command); after that get() is a
// mutex-protected string copy.
class MonitorPoller {
public:
  ~MonitorPoller() {
    {
      std::lock_guard<std::mutex> lk(m_);
      stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable())
      thread_.join();
  }

  std::string get() {
    std::call_once(started_, [this] {
      value_ = queryDefaultMonitor();
      thread_ = std::thread([this] { loop(); });
    });
    std::lock_guard<std::mutex> lk(m_);
    return value_;
  }

private:
  void loop() {
    std::unique_lock<std::mutex> lk(m_);
    while (!stop_) {
      if (cv_.wait_for(lk, std::chrono::seconds(MONITOR_POLL_SECS),
                       [this] { return stop_; }))
        break;
      lk.unlock();
      std::string v = queryDefaultMonitor();
      lk.lock();
      // Keep the last good value if pactl hiccups or times out.
      if (!v.empty())
        value_ = std::move(v);
    }
  }

  std::mutex m_;
  std::condition_variable cv_;
  std::once_flag started_;
  std::thread thread_;
  std::string value_;
  bool stop_ = false;
};
} // namespace

std::string detectMonitor() {
  static MonitorPoller poller;
  return poller.get();
}

std::string sourceLabel(const std::string &source) {
  return source == AudioCapture::MIC_SOURCE ? "default microphone" : source;
}

// ── Audio backend factory
// ─────────────────────────────────────────────────────

std::unique_ptr<AudioCapture> makeAudio(const std::string &backend,
                                        const std::string &source, int sr,
                                        int ch,
                                        AudioCapture::AudioCallback cb) {
#if defined(HAVE_PIPEWIRE) || defined(HAVE_PULSEAUDIO)
  auto try1 =
      [&](std::unique_ptr<AudioCapture> cap) -> std::unique_ptr<AudioCapture> {
    if (!cap->init(source, sr, ch))
      return nullptr;
    if (!cap->start(cb))
      return nullptr;
    return cap;
  };
#endif
  std::unique_ptr<AudioCapture> a;
#ifdef HAVE_PIPEWIRE
  if (backend == "auto" || backend == "pipewire") {
    a = try1(std::make_unique<PipeWireCapture>());
    if (a || backend == "pipewire")
      return a;
  }
#endif
#ifdef HAVE_PULSEAUDIO
  if (!a && (backend == "auto" || backend == "pulse"))
    a = try1(std::make_unique<PulseAudioCapture>());
#endif
  // Suppress "unused parameter" warnings when both backends are disabled.
  (void)backend;
  (void)source;
  (void)sr;
  (void)ch;
  (void)cb;
  return a;
}

// ── Audio startup with fallback chain ────────────────────────────────────────

void doStartAudio(const std::string &backend, const std::string &cli_source,
                  bool use_mic, int sample_rate, int channels,
                  FFTProcessor &fft, Config &cfg,
                  std::unique_ptr<AudioCapture> &audio,
                  std::string &active_source, std::string &bname) {
  AudioCapture::AudioCallback cb = [&fft](const float *s, std::size_t n,
                                          int ch) { fft.addSamples(s, n, ch); };

  if (!cli_source.empty()) {
    active_source = cli_source;
    audio = makeAudio(backend, active_source, sample_rate, channels, cb);
  } else if (use_mic) {
    active_source = AudioCapture::MIC_SOURCE;
    audio = makeAudio(backend, active_source, sample_rate, channels, cb);
  } else {
    active_source = "";
    audio = makeAudio(backend, active_source, sample_rate, channels, cb);
    if (!audio && !cfg.last_source.empty()) {
      active_source = cfg.last_source;
      audio = makeAudio(backend, active_source, sample_rate, channels, cb);
    }
    if (!audio) {
      active_source = detectMonitor();
      if (!active_source.empty())
        audio = makeAudio(backend, active_source, sample_rate, channels, cb);
    }
    if (!audio) {
      active_source = "";
      audio = makeAudio(backend, active_source, sample_rate, channels, cb);
    }
  }

  if (audio) {
    bname = audio->backendName();
    if (!active_source.empty() && !use_mic) {
      cfg.last_source = active_source;
      cfg.saveState();
    }
  }
}

// ── FFT configuration
// ─────────────────────────────────────────────────────────

void applyFFTConfig(FFTProcessor &fft, const Config &cfg) {
  fft.setSensitivity(cfg.sensitivity);
  fft.setAutoSens(cfg.auto_sens);
  fft.setGravity(cfg.gravity);
  fft.setMonstercat(cfg.monstercat);
  fft.setHighCutoff(cfg.high_cutoff);
  fft.setRiseFactor(cfg.rise_factor);
  fft.setBassSmooth(cfg.bass_smooth);
  fft.setAWeighting(cfg.a_weighting);
  fft.setNoiseGate(cfg.noise_gate);
  fft.setAutoMono(cfg.auto_mono);
}
