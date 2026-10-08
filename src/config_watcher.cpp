#include "config_watcher.h"

#include "config.h"

#include <cstring>
#include <filesystem>
#include <system_error>

#ifdef __linux__
#include <climits> // NAME_MAX
#include <sys/inotify.h>
#include <unistd.h>
#endif

bool ConfigWatcher::isThemeFileName(const char *name) {
  if (!name)
    return false;
  const std::size_t n = std::strlen(name);
  return n >= 7 && // shortest: "a.theme"
         std::strcmp(name + n - 6, ".theme") == 0 && name[0] != '.';
}

#ifdef __linux__

ConfigWatcher::~ConfigWatcher() {
  if (fd_ >= 0)
    close(fd_);
}

bool ConfigWatcher::start(const std::string &config_path,
                          const std::string &themes_dir) {
  const std::size_t slash = config_path.rfind('/');
  const std::string dir = config_path.substr(0, slash);
  cfg_file_ = config_path.substr(slash + 1);

  // Recursive: ~/.config itself may not exist yet (fresh container / new
  // user).  A plain mkdir() of .../viz then fails and the watch is silently
  // never set up.  Creating themes/ lets users drop in their first theme.
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::filesystem::create_directories(themes_dir, ec);

  fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (fd_ < 0)
    return false;
  wd_cfg_ = inotify_add_watch(fd_, dir.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO);
  if (wd_cfg_ < 0) {
    close(fd_);
    fd_ = -1;
    return false;
  }
  // IN_DELETE covers rm; IN_CLOSE_WRITE covers saves; IN_MOVED_TO covers the
  // atomic-rename saves most editors use.  A missing themes/ dir is non-fatal.
  wd_themes_ = inotify_add_watch(fd_, themes_dir.c_str(),
                                 IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE);
  return true;
}

ConfigWatcher::Events ConfigWatcher::poll() {
  Events out;
  if (fd_ < 0)
    return out;

  alignas(struct inotify_event) char
      buf[sizeof(struct inotify_event) + NAME_MAX + 1];
  ssize_t len;
  bool cfg_seen = false;
  while ((len = read(fd_, buf, sizeof(buf))) > 0) {
    for (const char *p = buf; p < buf + len;) {
      const auto *ev = reinterpret_cast<const struct inotify_event *>(p);
      if (ev->wd == wd_cfg_ && ev->len > 0 && cfg_file_ == ev->name) {
        // Our own save() fires this too.  If the file on disk is exactly what
        // we last wrote there is nothing to reload.  One report per batch
        // (CLOSE_WRITE + MOVED_TO often arrive together).
        if (!cfg_seen &&
            Config::currentFileDigest() != Config::lastSavedDigest()) {
          out.config_changed = true;
          cfg_seen = true;
        }
      } else if (ev->wd == wd_themes_ && ev->len > 0 &&
                 isThemeFileName(ev->name)) {
        out.themes_changed = true;
      }
      p += sizeof(struct inotify_event) + ev->len;
    }
  }
  return out;
}

#else // ── non-Linux: no inotify; SIGUSR1 reload still works ────────────────

ConfigWatcher::~ConfigWatcher() = default;
bool ConfigWatcher::start(const std::string &, const std::string &) {
  return false;
}
ConfigWatcher::Events ConfigWatcher::poll() { return {}; }

#endif
