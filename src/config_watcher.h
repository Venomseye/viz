#pragma once
#include <string>

/// Watches the config file and the themes directory (inotify on Linux; a
/// no-op elsewhere, where SIGUSR1 still works).
class ConfigWatcher {
public:
  struct Events {
    bool config_changed = false; // config edited by something other than us
    bool themes_changed = false; // a .theme file was added, edited or removed
  };

  ConfigWatcher() = default;
  ~ConfigWatcher();
  ConfigWatcher(const ConfigWatcher &) = delete;
  ConfigWatcher &operator=(const ConfigWatcher &) = delete;

  /// Create both directories if needed and start watching.  Returns false if
  /// watching is unavailable (the program then simply has no auto-reload).
  bool start(const std::string &config_path, const std::string &themes_dir);

  /// Non-blocking: collect everything that happened since the last call.
  /// A change caused by our own Config::save() is NOT reported.
  Events poll();

  /// Editor temp/swap/hidden files (ocean.theme~, .ocean.theme.swp, ...) are
  /// not themes.
  static bool isThemeFileName(const char *name);

private:
  int fd_ = -1;
  int wd_cfg_ = -1;
  int wd_themes_ = -1;
  std::string cfg_file_;
};
