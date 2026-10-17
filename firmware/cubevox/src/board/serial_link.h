// The USB CDC port as the JSON protocol uses it (spec 2026-10-12 serial protocol). Reads bytes,
// frames JSON lines, and sends output through a bounded queue so no print ever waits on the host.

#pragma once

#include <Arduino.h>

#include "core/protocol.h"

// How long the queue gets to leave before the box resets, so the last reply and log lines reach the host.
constexpr uint32_t kFlushBeforeResetMs = 100;

// Called for each complete request line. `nowMs` is the loop pass's clock read.
using LineHandler = void (*)(const char* line, uint32_t nowMs);

// Reads the pending bytes and calls `onLine` for each complete JSON line (NUL-terminated, valid
// during the call), then sends as much queued output as the port takes. Bytes outside a line are
// ignored and counted. A line over the length limit gets its error reply here.
void serialLinkPoll(uint32_t nowMs, LineHandler onLine);

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

// Bytes received outside a JSON line since boot.
uint32_t serialStrayBytes();
