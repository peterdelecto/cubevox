#include "board/serial_link.h"

#include "core/tx_queue.h"

namespace {

TxQueue gQueue;

class QueuePrint : public Print {
 public:
  size_t write(uint8_t byte) override {
    const char c = static_cast<char>(byte);
    gQueue.put(&c, 1);
    return 1;
  }
  size_t write(const uint8_t* data, size_t len) override {
    gQueue.put(reinterpret_cast<const char*>(data), len);
    return len;
  }
};

class QueueWriter : public protocol::Writer {
 public:
  void write(const char* text, size_t len) override { gQueue.put(text, len); }
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

}  // namespace

Print& serialOut() { return gPrint; }

protocol::Writer& serialWriter() { return gWriter; }

uint32_t serialDropped() { return gQueue.dropped(); }

bool serialFlush(uint32_t timeoutMs) {
  const uint32_t start = millis();
  for (;;) {
    drain();
    if (gQueue.pending() == 0) return true;
    if (millis() - start >= timeoutMs) return false;
  }
}

void serialLinkSessionStart() { gFramer.reset(); }

void serialLinkPoll(const SerialHandlers& handlers) {
  while (Serial.available() > 0) {
    switch (gFramer.feed(static_cast<uint8_t>(Serial.read()))) {
      case protocol::Framer::Kind::Bench: handlers.bench(gFramer.bench()); break;
      case protocol::Framer::Kind::Line: handlers.request(gFramer.line()); break;
      case protocol::Framer::Kind::TooLong: protocol::replyError(gWriter, 0, "line too long (max 768 bytes)"); break;
      case protocol::Framer::Kind::None: break;
    }
  }
  drain();
}
