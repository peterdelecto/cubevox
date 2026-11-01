// The sound source: Adam's clip on a loop, or the Mac's chosen input. The audio device calls into the
// block path from here, and the headless check pulls the clip without a device.

#pragma once

#include <stdint.h>

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "engine/common.h"

struct ma_device;

namespace host {

class AudioBlock;

class Source {
 public:
  Source();
  ~Source();
  Source(const Source&) = delete;
  Source& operator=(const Source&) = delete;

  // Decodes a WAV to mono f32 at the engine's sample rate. False when it cannot.
  bool loadClip(const std::string& path);

  // Looks for the clip beside the app (../../../samples from inside the bundle, also from the bundle
  // a symlink points at), then in the working directory. `argv0` is the program path as launched.
  bool findClip(const char* argv0);

  // Copies the next `n` clip samples, wrapping at the end. Silence when no clip is loaded.
  void readClip(float* dst, int n);

  // Starts the device playback only (stereo out, 48 kHz, period of one block), so a Bluetooth
  // headset keeps its music profile while the clip plays. Looks for a capture device by listing
  // the devices, without opening one. False when no output device starts.
  bool startDevice(AudioBlock& block);
  void stopDevice();

  // True when a capture device exists and the last duplex open did not fail, so MIC can be chosen.
  bool micAvailable() const { return micAvailable_; }

  // Picks the source. MIC reopens the device duplex (mic in, mono, and stereo out); CLIP reopens it
  // playback only. A duplex open that fails leaves the clip playing and clears micAvailable().
  void setUseMic(bool on);
  bool useMic() const { return useMic_.load(std::memory_order_relaxed); }

  const std::string& clipPath() const { return clipPath_; }

  // The device callback. Public so the C trampoline can reach it.
  void onAudio(float* playback, const float* capture, uint32_t frames);

 private:
  bool openDevice(bool duplex);
  void closeDevice();

  std::vector<float> clip_;
  size_t clipPos_ = 0;
  std::string clipPath_;

  std::unique_ptr<ma_device> device_;
  AudioBlock* block_ = nullptr;
  bool micAvailable_ = false;
  std::atomic<bool> useMic_{false};

  // The device asks for any number of frames; the block path takes whole blocks. Input fills a
  // block, and playback reads the block the last fill produced, one block behind.
  std::array<float, cv::kBlock> blockIn_ = {};
  std::array<float, cv::kBlock> blockOut_ = {};
  int fill_ = 0;
};

}  // namespace host
