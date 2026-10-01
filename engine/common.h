#pragma once

// Engine contract. Every header under engine/ compiles unchanged on the H7:
// float math only, std::array state, no heap and no I/O in process().

namespace cv {

constexpr int kSampleRate = 48000;
constexpr int kBlock = 64;

}  // namespace cv
