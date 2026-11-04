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
#include "signal_util.h"

namespace clips {

using Signal = std::vector<float>;
constexpr size_t kPad = cv::kSampleRate * 2 / 5;

using testutil::readWav;

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
