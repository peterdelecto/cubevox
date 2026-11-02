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

  // Main thread. Decodes a file to mono f32 at the engine's sample rate and swaps it in for the audio
  // callback, playing from its start. False, with the old clip still playing, when it does not decode.
  // The old buffer is freed once the callback has run twice since the swap.
  bool loadClip(const char* path);

  // The base name of the clip now playing, "" before the first load.
  const char* clipName() const { return clipName_.c_str(); }

  // The path of samples/<file> beside the app (../../../samples from inside the bundle, also from the
  // bundle a symlink points at), then in the working directory; "" when it is in neither. `argv0` is
  // the program path as launched.
  std::string locate(const char* argv0, const char* file) const;

  // The path of Adam's default clip as locate() finds it, "" when samples/ does not hold it.
  std::string locateDefault(const char* argv0) const;

  // Loads Adam's default clip from where locateDefault() finds it.
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

  // The device callback. Public so the C trampoline can reach it.
  void onAudio(float* playback, const float* capture, uint32_t frames);

 private:
  // One decoded clip. The id grows with each load, so the reader can tell a new clip from the old
  // one even when the allocator hands the new one an old address.
  struct Clip {
    std::vector<float> samples;
    uint64_t id = 0;
  };
  struct Retired {
    Clip* clip;
    uint64_t freeAtCallback;
  };

  bool openDevice(bool duplex);
  void closeDevice();
  void reclaim();

  // The audio side loads clip_ for each sample and restarts at 0 when the id changes. The main side
  // swaps the pointer, retires the old clip, and frees it once callbacks_ has passed the retire mark,
  // so a callback that was reading it has returned. With no device nothing reads, and it frees at once.
  std::atomic<Clip*> clip_{nullptr};
  std::atomic<uint64_t> callbacks_{0};
  std::vector<Retired> retired_;  // main thread only
  uint64_t nextId_ = 1;           // main thread only
  uint64_t playingId_ = 0;        // reader only
  size_t clipPos_ = 0;            // reader only
  std::string clipName_;

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
