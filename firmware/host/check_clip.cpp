#include "check_clip.h"

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

#include "engine/common.h"

namespace host {

namespace {

constexpr double kTwoPi = 6.283185307179586;
constexpr double kVibratoHz = 5.0;
constexpr double kVibratoCents = 15.0;
constexpr double kSwellHz = 1.0;      // the level swells once a second
constexpr double kSwellFloor = 0.4;   // the quietest point, as a share of the loudest
constexpr double kPeak = 0.5;         // -6 dBFS
constexpr double kFadeSeconds = 0.02;
constexpr double kTopHz = 5000.0;     // no harmonic above this
constexpr double kTiltGain = 0.03;    // the 1/k floor under the formant peaks

// Peaks in the harmonic amplitudes, a rough "ah".
struct Formant {
  double centreHz;
  double widthHz;
  double gain;
};
constexpr std::array<Formant, 3> kFormants = {{{700.0, 110.0, 1.0}, {1200.0, 160.0, 0.5}, {2600.0, 250.0, 0.12}}};

constexpr uint32_t kHeaderBytes = 44;
constexpr uint16_t kBitsPerSample = 16;

void put16(std::vector<uint8_t>& bytes, uint16_t v) {
  bytes.push_back(static_cast<uint8_t>(v & 0xFFu));
  bytes.push_back(static_cast<uint8_t>(v >> 8));
}

void put32(std::vector<uint8_t>& bytes, uint32_t v) {
  put16(bytes, static_cast<uint16_t>(v & 0xFFFFu));
  put16(bytes, static_cast<uint16_t>(v >> 16));
}

void putTag(std::vector<uint8_t>& bytes, const char* tag) {
  for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>(tag[i]));
}

// The amplitude of each harmonic of f0: the formant peaks over a falling floor.
std::vector<double> harmonicAmps(double f0Hz) {
  std::vector<double> amps;
  for (int k = 1; f0Hz * k < kTopHz; ++k) {
    const double hz = f0Hz * k;
    double a = kTiltGain / k;
    for (const Formant& f : kFormants) {
      const double z = (hz - f.centreHz) / f.widthHz;
      a += f.gain * std::exp(-0.5 * z * z);
    }
    amps.push_back(a);
  }
  return amps;
}

}  // namespace

bool writeVoicedClip(const std::string& path, double f0Hz, double seconds) {
  const size_t n = static_cast<size_t>(seconds * cv::kSampleRate);
  const std::vector<double> amps = harmonicAmps(f0Hz);
  std::vector<double> x(n, 0.0);
  double phase = 0.0;
  double peak = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / cv::kSampleRate;
    const double hz = f0Hz * std::exp2(kVibratoCents / 1200.0 * std::sin(kTwoPi * kVibratoHz * t));
    phase += kTwoPi * hz / cv::kSampleRate;
    double s = 0.0;
    for (size_t k = 0; k < amps.size(); ++k) s += amps[k] * std::sin(phase * static_cast<double>(k + 1));
    const double swell = kSwellFloor + (1.0 - kSwellFloor) * 0.5 * (1.0 - std::cos(kTwoPi * kSwellHz * t));
    x[i] = s * swell;
    peak = std::max(peak, std::fabs(x[i]));
  }
  const size_t fade = static_cast<size_t>(kFadeSeconds * cv::kSampleRate);
  const double scale = peak > 0.0 ? kPeak / peak : 0.0;

  const uint32_t dataBytes = static_cast<uint32_t>(n * sizeof(int16_t));
  std::vector<uint8_t> bytes;
  bytes.reserve(kHeaderBytes + dataBytes);
  putTag(bytes, "RIFF");
  put32(bytes, kHeaderBytes - 8 + dataBytes);
  putTag(bytes, "WAVE");
  putTag(bytes, "fmt ");
  put32(bytes, 16);
  put16(bytes, 1);  // PCM
  put16(bytes, 1);  // mono
  put32(bytes, static_cast<uint32_t>(cv::kSampleRate));
  put32(bytes, static_cast<uint32_t>(cv::kSampleRate) * kBitsPerSample / 8);
  put16(bytes, kBitsPerSample / 8);
  put16(bytes, kBitsPerSample);
  putTag(bytes, "data");
  put32(bytes, dataBytes);
  for (size_t i = 0; i < n; ++i) {
    double v = x[i] * scale;
    if (i < fade) v *= static_cast<double>(i) / static_cast<double>(fade);
    if (n - 1 - i < fade) v *= static_cast<double>(n - 1 - i) / static_cast<double>(fade);
    put16(bytes, static_cast<uint16_t>(static_cast<int16_t>(std::lround(v * 32767.0))));
  }

  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) return false;
  const bool wrote = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
  return std::fclose(file) == 0 && wrote;
}

SyntheticClips::SyntheticClips() {
  const char* tmp = std::getenv("TMPDIR");
  std::string pattern = tmp != nullptr && *tmp != '\0' ? tmp : "/tmp";
  if (pattern.back() != '/') pattern += '/';
  pattern += "cubevox-check-XXXXXX";
  std::vector<char> buffer(pattern.begin(), pattern.end());
  buffer.push_back('\0');
  if (mkdtemp(buffer.data()) == nullptr) {
    std::fprintf(stderr, "[ERROR] check clips: cannot make a temporary directory from %s\n", pattern.c_str());
    return;
  }
  dir_ = buffer.data();
  const std::string first = dir_ + "/" + kFirstName;
  const std::string second = dir_ + "/" + kSecondName;
  if (writeVoicedClip(first, 150.0, 4.0) && writeVoicedClip(second, 220.0, 3.0)) {
    first_ = first;
    second_ = second;
  } else {
    std::fprintf(stderr, "[ERROR] check clips: cannot write the clips in %s\n", dir_.c_str());
  }
}

SyntheticClips::~SyntheticClips() {
  if (dir_.empty()) return;
  unlink((dir_ + "/" + kFirstName).c_str());
  unlink((dir_ + "/" + kSecondName).c_str());
  rmdir(dir_.c_str());
}

}  // namespace host
