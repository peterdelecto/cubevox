// The USB CDC port as the JSON protocol and the bench letters share it (spec 2026-10-12 serial
// protocol). Reads bytes, splits JSON lines from bench letters, and sends output through a bounded
// queue so no print ever waits on the host.

#pragma once

#include <Arduino.h>

#include "core/protocol.h"

struct SerialHandlers {
  void (*bench)(char letter);
  void (*request)(const char* line);  // one NUL-terminated JSON line, valid during the call
};

// Reads the pending bytes and calls a handler for each bench letter and each complete line, then
// sends as much queued output as the port takes. A line over the length limit gets its error reply here.
void serialLinkPoll(const SerialHandlers& handlers);

// Drops a line the last host left unfinished, so it can't swallow the new session's first bytes.
void serialLinkSessionStart();

// Everything the firmware prints goes here instead of Serial. Whole lines that do not fit the queue are dropped.
Print& serialOut();

// Reply sink for the protocol, on the same queue.
protocol::Writer& serialWriter();

// Sends queued output, waiting up to timeoutMs for the port. Returns true when the queue is empty.
// For the moments before a reset, DFU entry or fault.
bool serialFlush(uint32_t timeoutMs);

// Lines dropped because the queue was full.
uint32_t serialDropped();
