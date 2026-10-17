#include "board/serial_link.h"

#include <stdio.h>

#include "cdc_queue.h"
#include "core/tx_queue.h"

namespace {

// The core's CDC receive queue must hold every byte that arrives during one loop() pass. When it
// fills, the core arms a zero-length receive and the next packet wraps the queue, which drops or
// replays up to a queue's worth of bytes. Two full requests fit; build.sh sets the packet count.
static_assert(CDC_RECEIVE_QUEUE_BUFFER_SIZE >= 2 * protocol::kMaxLine,
              "raise CDC_RECEIVE_QUEUE_BUFFER_PACKET_NUMBER in build.sh to hold two maximum-length requests");

TxQueue gQueue;

class QueuePrint : public Print {
 public:
  size_t write(uint8_t byte) override {
    const char c = static_cast<char>(byte);
    gQueue.put(&c, 1, TxClass::Log);
    return 1;
  }
  size_t write(const uint8_t* data, size_t len) override {
    gQueue.put(reinterpret_cast<const char*>(data), len, TxClass::Log);
    return len;
  }
};

class QueueWriter : public protocol::Writer {
 public:
  void write(const char* text, size_t len) override { gQueue.put(text, len, TxClass::Reply); }
};

QueuePrint gPrint;
QueueWriter gWriter;
protocol::Framer gFramer;

// Hands the port what it has room for. USBSerial::write loops while the CDC queue is full, so
// it only ever gets as many bytes as availableForWrite() reports. A write that takes nothing
// means no host is connected, and the output is discarded.
void drain() {
  for (;;) {
    const char* run;
    const size_t ready = gQueue.peek(run);
    if (ready == 0) return;
    const int room = Serial.availableForWrite();
    if (room <= 0) return;
    const size_t n = ready < static_cast<size_t>(room) ? ready : static_cast<size_t>(room);
    const size_t sent = Serial.write(reinterpret_cast<const uint8_t*>(run), n);
    if (sent == 0) {
      gQueue.discard();
      return;
    }
    gQueue.consume(sent);
  }
}

void replyTooLong() {
  char reason[protocol::kErrorBytes];
  snprintf(reason, sizeof reason, "line too long (max %u bytes)", static_cast<unsigned>(protocol::kMaxLine));
  protocol::replyError(gWriter, 0, reason);
}

}  // namespace

Print& serialOut() { return gPrint; }

protocol::Writer& serialWriter() { return gWriter; }

uint32_t serialDropped() { return gQueue.dropped(); }

uint32_t serialStrayBytes() { return gFramer.strayBytes(); }

bool serialFlush(uint32_t timeoutMs) {
  const uint32_t start = millis();
  for (;;) {
    drain();
    if (gQueue.pending() == 0) return true;
    if (millis() - start >= timeoutMs) return false;
  }
}

void serialLinkSessionStart() { gFramer.reset(); }

void serialLinkPoll(uint32_t nowMs, LineHandler onLine) {
  while (Serial.available() > 0) {
    switch (gFramer.feed(static_cast<uint8_t>(Serial.read()))) {
      case protocol::Framer::Kind::Line: onLine(gFramer.line(), nowMs); break;
      case protocol::Framer::Kind::TooLong: replyTooLong(); break;
      case protocol::Framer::Kind::BadByte: protocol::replyError(gWriter, 0, "line contains a NUL byte"); break;
      case protocol::Framer::Kind::None: break;
    }
  }
  drain();
}
