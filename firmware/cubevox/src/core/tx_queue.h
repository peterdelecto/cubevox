// Bounded transmit queue for the serial port. Producers append text; the board drains it with
// non-blocking writes. The unit is the line: when a line does not fit, the whole line is dropped
// and counted, so the host never sees half a reply. Spec 2026-10-12-serial-protocol-design.md.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <array>

// What a line is. The tail of the queue is kept free of log lines, so a reply still fits when
// diagnostics have filled the rest and log lines drop first.
enum class TxClass : uint8_t { Log, Reply };

class TxQueue {
 public:
  static constexpr size_t kCapacity = 8192;
  static constexpr size_t kReplyReserve = 1024;

  // Appends text of one class. A line ends at '\n'; bytes after the last '\n' are held back from
  // draining until their line ends. A line is written by one producer, so it keeps one class.
  void put(const char* text, size_t len, TxClass cls);

  // The next contiguous run of finished lines. Returns its length, 0 when nothing is ready.
  size_t peek(const char*& run) const;
  void consume(size_t len);

  size_t pending() const { return commit_ - head_; }
  uint32_t dropped() const { return dropped_; }

  // Forgets every finished line, for when no host is there to read it. Not counted as dropped.
  // A line still open keeps its bytes, so its remainder does not reach the host as a fragment.
  void discard();

 private:
  void putByte(char c, size_t limit);

  std::array<char, kCapacity> buf_;
  size_t head_ = 0;    // next byte to send (absolute count)
  size_t commit_ = 0;  // end of the finished lines
  size_t tail_ = 0;    // end of everything written
  bool failed_ = false;  // the open line overflowed; its remaining bytes are skipped
  uint32_t dropped_ = 0;
};
