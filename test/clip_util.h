#pragma once

// Adam's three clips for the card level checks. Each clip comes back behind 0.4 s of silence so an enable
// fade settles first. The list is empty when samples/ is absent, which skips the rows.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "engine/common.h"

namespace clips {

using Signal = std::vector<float>;
constexpr size_t kPad = cv::kSampleRate * 2 / 5;

// 16-bit mono PCM WAV, any chunks before data. Empty on a missing or other file.
inline Signal readWav(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  std::vector<char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0) return {};
  size_t pos = 12;
  while (pos + 8 <= b.size()) {
    uint32_t len;
    std::memcpy(&len, &b[pos + 4], 4);
    if (std::memcmp(&b[pos], "data", 4) == 0) {
      const size_t n = std::min<size_t>(len, b.size() - pos - 8) / 2;
      Signal x(n);
      for (size_t i = 0; i < n; ++i) {
        int16_t s;
        std::memcpy(&s, &b[pos + 8 + 2 * i], 2);
        x[i] = static_cast<float>(s) / 32768.0f;
      }
      return x;
    }
    pos += 8 + len + (len & 1);
  }
  return {};
}

// `whole` is false when samples/ exists but a clip is missing.
inline std::vector<Signal> load(const char* dir, bool& whole) {
  const char* names[] = {"adam_vox_sample_3s", "adam_vox_sample2_4s", "adam_vox_sample3_5s"};
  std::vector<Signal> out;
  whole = true;
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) return out;
  for (const char* name : names) {
    const Signal clip = readWav(std::string(dir) + "/" + name + ".wav");
    if (clip.empty()) {
      whole = false;
      continue;
    }
    Signal padded(kPad, 0.0f);
    padded.insert(padded.end(), clip.begin(), clip.end());
    out.push_back(padded);
  }
  return out;
}

}  // namespace clips
