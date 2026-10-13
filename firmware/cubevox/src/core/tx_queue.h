// Bounded transmit queue for the serial port. Producers append text; the board drains it with
// non-blocking writes. The unit is the line: when a line does not fit, the whole line is dropped
// and counted, so the host never sees half a reply. Spec 2026-10-12-serial-protocol-design.md.

#pragma once

#include <stddef.h>
#include <stdint.h>

class TxQueue {
 public:
  static constexpr size_t kCapacity = 8192;

  // Appends text. A line ends at '\n'; bytes after the last '\n' are held back from draining
  // until their line ends.
  void put(const char* text, size_t len);

  // The next contiguous run of finished lines. Returns its length, 0 when nothing is ready.
  size_t peek(const char*& run) const;
  void consume(size_t len);

  size_t pending() const { return commit_ - head_; }
  uint32_t dropped() const { return dropped_; }

  // Forgets everything unsent, for when no host is there to read it. Not counted as dropped.
  void discard();

 private:
  void putByte(char c);

  char buf_[kCapacity];
  size_t head_ = 0;    // next byte to send (absolute count)
  size_t commit_ = 0;  // end of the finished lines
  size_t tail_ = 0;    // end of everything written
  bool failed_ = false;  // the open line overflowed; its remaining bytes are skipped
  uint32_t dropped_ = 0;
};
