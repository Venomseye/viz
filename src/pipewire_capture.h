#pragma once
#ifdef HAVE_PIPEWIRE

#include "audio_capture.h"
#include <atomic>
#include <string>
#include <thread>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>

class PipeWireCapture final : public AudioCapture {
public:
  PipeWireCapture();
  ~PipeWireCapture() override;

  PipeWireCapture(const PipeWireCapture &) = delete;
  PipeWireCapture &operator=(const PipeWireCapture &) = delete;

  bool init(const std::string &source = "", int sample_rate = 44100,
            int channels = 1) override;
  bool start(AudioCallback cb) override;
  void stop() override;
  bool hasFailed() const override { return failed_.load(); }

  std::string backendName() const override { return "PipeWire"; }
  int sampleRate() const override { return sample_rate_; }
  int channels() const override { return channels_; }

private:
  struct pw_main_loop *loop_{nullptr};
  struct pw_context *ctx_{nullptr};
  struct pw_core *core_{nullptr};
  struct pw_stream *stream_{nullptr};

  struct spa_hook stream_listener_{};
  struct pw_stream_events stream_events_{};

  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> failed_{false};

  AudioCallback callback_;
  std::string source_;
  bool mic_{false}; // capture the default input instead of the system mix
  int sample_rate_{44100};
  int channels_{1};
  std::atomic<int> neg_channels_{1}; // as negotiated; set in onParamChanged

  static void onProcess(void *ud);
  static void onParamChanged(void *ud, uint32_t id, const struct spa_pod *p);
  static void onStreamState(void *ud, enum pw_stream_state old_s,
                            enum pw_stream_state new_s, const char *error);
};

#endif // HAVE_PIPEWIRE
