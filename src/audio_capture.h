#pragma once
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

/// Abstract audio capture backend.
/// Callbacks are invoked from a dedicated capture thread.
class AudioCapture {
public:
  /// Called from the capture thread with `count` interleaved float samples
  /// (count = frames * channels).  Pointer + length instead of a std::vector
  /// so the (real-time) PipeWire thread never allocates; the pointer is only
  /// valid for the duration of the call.
  using AudioCallback = std::function<void(const float *samples,
                                           std::size_t count, int channels)>;

  /// Pseudo source name meaning "the default MICROPHONE / default input
  /// device" (as opposed to "" = what's playing, i.e. the default output's
  /// monitor).  Not a real device name, so it can't clash with one.
  static constexpr const char *MIC_SOURCE = "@default-mic@";

  virtual ~AudioCapture() = default;

  /// Store configuration (does not open device).
  virtual bool init(const std::string &source = "", int sample_rate = 44100,
                    int channels = 1) = 0;

  /// Open device and start capture thread.
  virtual bool start(AudioCallback cb) = 0;

  /// Stop capture and release device.  Safe to call multiple times.
  virtual void stop() = 0;

  /// Returns true if the capture thread exited due to an error
  /// (device disconnected, server died, etc.).  Checked by the main
  /// loop to trigger a reconnect.
  virtual bool hasFailed() const = 0;

  virtual std::string backendName() const = 0;
  virtual int sampleRate() const = 0;
  virtual int channels() const = 0;
};
