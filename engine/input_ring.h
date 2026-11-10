#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

// A card's ring of its own input, newest sample at head() - 1. A card that cuts grains or taps from what
// reaches its slot writes each chunk here before it reads. A stopped card calls rest() each block, which
// zeroes a slice, so the clear() at its next start finds most of the ring clear already.

namespace cv {

template <int Len>
class InputRing {
 public:
  using Array = std::array<float, static_cast<size_t>(Len)>;

  // The floats one rest() call zeroes.
  static constexpr int kRestClearFloats = 512;

  // Writes m samples, m at most Len, and returns the ring index of the first. The head moves past them.
  int write(const float* in, int m) {
    zero_ = 0;
    const int first = head_;
    int at = head_;
    for (int done = 0; done < m;) {
      const int run = m - done < Len - at ? m - done : Len - at;
      std::memcpy(&ring_[static_cast<size_t>(at)], in + done, sizeof(float) * static_cast<size_t>(run));
      at = at + run == Len ? 0 : at + run;
      done += run;
    }
    head_ = at;
    filled_ = filled_ + m < Len ? filled_ + m : Len;
    return first;
  }

  // Zeroes the next kRestClearFloats floats, in index order.
  void rest() { clearSome(kRestClearFloats); }

  // Zeroes what rest() left and puts the head at 0.
  void clear() {
    clearSome(Len);
    head_ = 0;
    filled_ = 0;
  }

  const Array& data() const { return ring_; }
  int head() const { return head_; }

  // The samples written since the last clear(), at most Len. A read deeper than this finds the clear's zeros.
  int filled() const { return filled_; }

  // The floats the clears have zeroed since construction, for the tests. It wraps.
  uint32_t clearedForTest() const { return cleared_; }

 private:
  // Zeroes up to `most` more floats, from where the last clear stopped. A write starts the walk over at
  // index 0.
  void clearSome(int most) {
    const int n = most < Len - zero_ ? most : Len - zero_;
    std::fill_n(ring_.begin() + zero_, n, 0.0f);
    zero_ += n;
    cleared_ += static_cast<uint32_t>(n);
  }

  Array ring_{};
  int head_ = 0;
  int filled_ = 0;
  int zero_ = Len;  // where the next clear starts; Len when the walk has covered the ring
  uint32_t cleared_ = 0;
};

}  // namespace cv
