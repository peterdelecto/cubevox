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

constexpr const char* kClipFile = "samples/adam_vox_sample3_5s.wav";

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

void dataCallback(ma_device* device, void* output, const void* input, ma_uint32 frames) {
  static_cast<Source*>(device->pUserData)
      ->onAudio(static_cast<float*>(output), static_cast<const float*>(input), frames);
}

}  // namespace

Source::Source() = default;

Source::~Source() { stopDevice(); }

bool Source::loadClip(const std::string& path) {
  ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, kChannelsCapture, cv::kSampleRate);
  ma_decoder decoder;
  if (ma_decoder_init_file(path.c_str(), &cfg, &decoder) != MA_SUCCESS) {
    std::fprintf(stderr, "[ERROR] clip: cannot open %s\n", path.c_str());
    return false;
  }
  ma_uint64 frames = 0;
  ma_decoder_get_length_in_pcm_frames(&decoder, &frames);
  if (frames == 0) {
    ma_decoder_uninit(&decoder);
    std::fprintf(stderr, "[ERROR] clip: %s is empty\n", path.c_str());
    return false;
  }
  std::vector<float> samples(frames);
  ma_uint64 read = 0;
  ma_decoder_read_pcm_frames(&decoder, samples.data(), frames, &read);
  ma_decoder_uninit(&decoder);
  samples.resize(read);
  clip_ = std::move(samples);
  clipPos_ = 0;
  clipPath_ = path;
  return !clip_.empty();
}

bool Source::findClip(const char* argv0) {
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
    for (const char* up : kUpFromBundle) candidates.push_back(dir + up + kClipFile);
  }
  candidates.push_back(kClipFile);  // the working directory
  for (const std::string& path : candidates) {
    if (access(path.c_str(), R_OK) == 0) return loadClip(path);
  }
  std::fprintf(stderr, "[ERROR] clip: %s not found beside the app or in the working directory\n", kClipFile);
  return false;
}

void Source::readClip(float* dst, int n) {
  if (clip_.empty()) {
    std::memset(dst, 0, sizeof(float) * static_cast<size_t>(n));
    return;
  }
  for (int i = 0; i < n; ++i) {
    dst[i] = clip_[clipPos_];
    if (++clipPos_ >= clip_.size()) clipPos_ = 0;
  }
}

void Source::onAudio(float* playback, const float* capture, uint32_t frames) {
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

bool Source::startDevice(AudioBlock& block) {
  block_ = &block;
  ma_device_config cfg = ma_device_config_init(ma_device_type_duplex);
  cfg.capture.format = ma_format_f32;
  cfg.capture.channels = kChannelsCapture;
  cfg.playback.format = ma_format_f32;
  cfg.playback.channels = kChannelsPlayback;
  cfg.sampleRate = cv::kSampleRate;
  cfg.periodSizeInFrames = cv::kBlock;
  cfg.dataCallback = dataCallback;
  cfg.pUserData = this;

  device_ = std::make_unique<ma_device>();
  micAvailable_ = ma_device_init(nullptr, &cfg, device_.get()) == MA_SUCCESS;
  if (!micAvailable_) {
    std::fprintf(stderr, "[WARN] audio: no input device, the clip plays\n");
    cfg.deviceType = ma_device_type_playback;
    if (ma_device_init(nullptr, &cfg, device_.get()) != MA_SUCCESS) {
      std::fprintf(stderr, "[ERROR] audio: no output device\n");
      device_.reset();
      return false;
    }
  }
  if (ma_device_start(device_.get()) != MA_SUCCESS) {
    std::fprintf(stderr, "[ERROR] audio: the device did not start\n");
    ma_device_uninit(device_.get());
    device_.reset();
    micAvailable_ = false;
    return false;
  }
  return true;
}

void Source::stopDevice() {
  if (!device_) return;
  ma_device_uninit(device_.get());
  device_.reset();
  micAvailable_ = false;
}

}  // namespace host
