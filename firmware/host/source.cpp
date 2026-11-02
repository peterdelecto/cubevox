#include "source.h"

#include <limits.h>
#include <mach-o/dyld.h>
#include <stdlib.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

#include <miniaudio.h>

#include "audio_block.h"

namespace host {

namespace {

constexpr const char* kSampleDir = "samples/";
constexpr const char* kClipFile = "adam_vox_sample3_5s.wav";

// Longest clip kept, so a long file cannot take the Mac's memory.
constexpr ma_uint64 kMaxClipFrames = 600ull * cv::kSampleRate;
constexpr ma_uint64 kReadChunkFrames = 4096;

// The callbacks that must pass after a swap before the old clip may be freed. One may be reading it
// when the swap lands, and the next starts after that one returns.
constexpr uint64_t kRetireCallbacks = 2;

// From the executable inside the bundle to the directory that holds samples/. Three levels up is the
// bundle's parent; four reach the repository root when the bundle sits in build/.
constexpr const char* kUpFromBundle[] = {"/../../../", "/../../../../"};
constexpr const char* kInsideBundle = ".app/Contents/MacOS";

bool endsWith(const std::string& s, const std::string& tail) {
  return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

constexpr ma_uint32 kChannelsCapture = 1;
constexpr ma_uint32 kChannelsPlayback = 2;

std::string parentDir(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

// Lists the devices once and says whether any can capture. Opening none keeps a Bluetooth headset
// out of its call profile until MIC is chosen.
bool hasCaptureDevice() {
  const std::unique_ptr<ma_context> context = std::make_unique<ma_context>();
  if (ma_context_init(nullptr, 0, nullptr, context.get()) != MA_SUCCESS) return false;
  ma_device_info* playback = nullptr;
  ma_uint32 playbackCount = 0;
  ma_device_info* capture = nullptr;
  ma_uint32 captureCount = 0;
  const bool listed =
      ma_context_get_devices(context.get(), &playback, &playbackCount, &capture, &captureCount) == MA_SUCCESS;
  ma_context_uninit(context.get());
  return listed && captureCount > 0;
}

void dataCallback(ma_device* device, void* output, const void* input, ma_uint32 frames) {
  static_cast<Source*>(device->pUserData)
      ->onAudio(static_cast<float*>(output), static_cast<const float*>(input), frames);
}

}  // namespace

Source::Source() = default;

Source::~Source() {
  stopDevice();
  reclaim();
  delete clip_.load();
}

// Frees the retired clips that no callback can still be reading.
void Source::reclaim() {
  const uint64_t passed = callbacks_.load(std::memory_order_acquire);
  for (size_t i = 0; i < retired_.size();) {
    if (device_ == nullptr || passed >= retired_[i].freeAtCallback) {
      delete retired_[i].clip;
      retired_[i] = retired_.back();
      retired_.pop_back();
    } else {
      ++i;
    }
  }
}

bool Source::loadClip(const char* path) {
  ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, kChannelsCapture, cv::kSampleRate);
  ma_decoder decoder;
  if (ma_decoder_init_file(path, &cfg, &decoder) != MA_SUCCESS) {
    std::fprintf(stderr, "[ERROR] clip: cannot open %s\n", path);
    return false;
  }
  std::unique_ptr<Clip> clip = std::make_unique<Clip>();
  std::vector<float>& samples = clip->samples;
  for (;;) {
    const size_t at = samples.size();
    samples.resize(at + kReadChunkFrames);
    ma_uint64 read = 0;
    ma_decoder_read_pcm_frames(&decoder, samples.data() + at, kReadChunkFrames, &read);
    samples.resize(at + read);
    if (read < kReadChunkFrames || samples.size() >= kMaxClipFrames) break;
  }
  ma_decoder_uninit(&decoder);
  if (samples.empty()) {
    std::fprintf(stderr, "[ERROR] clip: %s holds no audio\n", path);
    return false;
  }
  clip->id = nextId_++;
  Clip* old = clip_.exchange(clip.release(), std::memory_order_acq_rel);
  if (old != nullptr) retired_.push_back({old, callbacks_.load(std::memory_order_acquire) + kRetireCallbacks});
  const char* slash = std::strrchr(path, '/');
  clipName_ = slash != nullptr ? slash + 1 : path;
  reclaim();
  return true;
}

std::string Source::locate(const char* argv0, const char* file) const {
  const std::string relative = std::string(kSampleDir) + file;
  std::vector<std::string> candidates;
  char raw[PATH_MAX] = {};
  uint32_t size = sizeof raw;
  std::vector<std::string> exeDirs;
  if (_NSGetExecutablePath(raw, &size) == 0) {
    exeDirs.push_back(parentDir(raw));
    char real[PATH_MAX] = {};
    if (realpath(raw, real) != nullptr && parentDir(real) != exeDirs.front()) exeDirs.push_back(parentDir(real));
  } else if (argv0 != nullptr) {
    exeDirs.push_back(parentDir(argv0));
  }
  for (const std::string& dir : exeDirs) {
    if (!endsWith(dir, kInsideBundle)) continue;  // a bare executable has no bundle to climb out of
    for (const char* up : kUpFromBundle) candidates.push_back(dir + up + relative);
  }
  candidates.push_back(relative);  // the working directory
  for (const std::string& path : candidates) {
    if (access(path.c_str(), R_OK) == 0) return path;
  }
  return std::string();
}

std::string Source::locateDefault(const char* argv0) const { return locate(argv0, kClipFile); }

bool Source::findClip(const char* argv0) {
  const std::string path = locateDefault(argv0);
  if (path.empty()) {
    std::fprintf(stderr, "[ERROR] clip: %s%s not found beside the app or in the working directory\n", kSampleDir,
                 kClipFile);
    return false;
  }
  return loadClip(path.c_str());
}

// Takes the clip now in place, and starts it over when it is not the one played last.
void Source::readClip(float* dst, int n) {
  const Clip* clip = clip_.load(std::memory_order_acquire);
  if (clip == nullptr) {
    std::memset(dst, 0, sizeof(float) * static_cast<size_t>(n));
    return;
  }
  if (clip->id != playingId_) {
    playingId_ = clip->id;
    clipPos_ = 0;
  }
  for (int i = 0; i < n; ++i) {
    dst[i] = clip->samples[clipPos_];
    if (++clipPos_ >= clip->samples.size()) clipPos_ = 0;
  }
}

void Source::onAudio(float* playback, const float* capture, uint32_t frames) {
  callbacks_.fetch_add(1, std::memory_order_acq_rel);
  const bool mic = capture != nullptr && useMic_.load(std::memory_order_relaxed);
  for (uint32_t i = 0; i < frames; ++i) {
    if (mic) {
      blockIn_[static_cast<size_t>(fill_)] = capture[i];
    } else {
      readClip(&blockIn_[static_cast<size_t>(fill_)], 1);
    }
    const float y = blockOut_[static_cast<size_t>(fill_)];
    playback[kChannelsPlayback * i] = y;
    playback[kChannelsPlayback * i + 1] = y;
    if (++fill_ == cv::kBlock) {
      block_->process(blockIn_.data(), blockOut_.data(), cv::kBlock);
      fill_ = 0;
    }
  }
}

// Opens the device in `duplex` or playback-only form and starts it. The callback accepts any frame
// count. False, with no device left open, when either step fails.
bool Source::openDevice(bool duplex) {
  ma_device_config cfg = ma_device_config_init(duplex ? ma_device_type_duplex : ma_device_type_playback);
  cfg.capture.format = ma_format_f32;
  cfg.capture.channels = kChannelsCapture;
  cfg.playback.format = ma_format_f32;
  cfg.playback.channels = kChannelsPlayback;
  cfg.sampleRate = cv::kSampleRate;
  cfg.periodSizeInFrames = cv::kBlock;
  cfg.dataCallback = dataCallback;
  cfg.pUserData = this;

  device_ = std::make_unique<ma_device>();
  if (ma_device_init(nullptr, &cfg, device_.get()) != MA_SUCCESS) {
    device_.reset();
    return false;
  }
  if (ma_device_start(device_.get()) != MA_SUCCESS) {
    ma_device_uninit(device_.get());
    device_.reset();
    return false;
  }
  return true;
}

void Source::closeDevice() {
  if (!device_) return;
  ma_device_uninit(device_.get());
  device_.reset();
}

bool Source::startDevice(AudioBlock& block) {
  block_ = &block;
  micAvailable_ = hasCaptureDevice();
  if (!micAvailable_) std::fprintf(stderr, "[WARN] audio: no input device, the clip plays\n");
  if (openDevice(false)) return true;
  std::fprintf(stderr, "[ERROR] audio: no output device\n");
  micAvailable_ = false;
  return false;
}

void Source::stopDevice() {
  closeDevice();
  micAvailable_ = false;
}

// The device is closed while it reopens, so the callback never runs across the change. A duplex
// open that fails (no permission, the input went away) dims MIC and goes back to the clip.
void Source::setUseMic(bool on) {
  if (on == useMic_.load(std::memory_order_relaxed)) return;
  if (!device_) {
    useMic_.store(on && micAvailable_, std::memory_order_relaxed);
    return;
  }
  if (on && !micAvailable_) return;
  closeDevice();
  if (openDevice(on)) {
    useMic_.store(on, std::memory_order_relaxed);
    return;
  }
  useMic_.store(false, std::memory_order_relaxed);
  if (on) {
    std::fprintf(stderr, "[WARN] audio: the input did not open, the clip plays\n");
    micAvailable_ = false;
  }
  if (!openDevice(false)) std::fprintf(stderr, "[ERROR] audio: no output device\n");
}

}  // namespace host
