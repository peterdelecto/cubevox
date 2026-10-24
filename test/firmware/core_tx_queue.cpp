// Checks the serial transmit queue on the host. Covers line framing, overflow, the reply
// reserve and discard.

#include "support.h"

#include <cstddef>
#include <cstring>
#include <string>

#include "core/tx_queue.h"

namespace {

// Takes every finished line out of the queue.
std::string drainQueue(TxQueue& q) {
  const char* run;
  size_t got = 0;
  std::string seen;
  while ((got = q.peek(run)) > 0) {
    seen.append(run, got);
    q.consume(got);
  }
  return seen;
}

void checkTxQueue() {
  TxQueue q;
  const char* run;
  q.put("abc", 3, TxClass::Reply);
  check(q.pending() == 0 && q.peek(run) == 0, "tx: an unfinished line is held back");
  q.put("d\nef\n", 5, TxClass::Reply);
  check(q.pending() == 8 && q.peek(run) == 8 && std::memcmp(run, "abcd\nef\n", 8) == 0, "tx: finished lines are ready");
  q.consume(5);
  check(q.pending() == 3, "tx: consume advances");
  q.consume(3);

  // A line that does not fit is dropped whole, counted once, and the queue keeps working.
  const std::string big(TxQueue::kCapacity - 4, 'x');
  q.put("keep\n", 5, TxClass::Reply);
  q.put(big.data(), big.size(), TxClass::Reply);
  q.put("\nnext\n", 6, TxClass::Reply);
  check(q.dropped() == 1, "tx: an overflowing line is counted");
  check(drainQueue(q) == "keep\nnext\n", "tx: only whole lines survive an overflow");

  // Wrap-around: the ring hands out contiguous runs.
  for (int round = 0; round < 3; ++round) {
    const std::string line(TxQueue::kCapacity / 2 + 100, static_cast<char>('a' + round));
    q.put(line.data(), line.size(), TxClass::Reply);
    q.put("\n", 1, TxClass::Reply);
    check(drainQueue(q) == line + "\n", "tx: lines survive the ring wrapping");
  }
}

// Log lines stop at the free-tail line, so a reply still fits when diagnostics fill the queue.
void checkTxReplyReserve() {
  TxQueue q;
  const std::string logLine(99, 'l');
  int logged = 0;
  while (q.dropped() == 0 && logged < 1000) {
    q.put(logLine.data(), logLine.size(), TxClass::Log);
    q.put("\n", 1, TxClass::Log);
    ++logged;
  }
  check(q.dropped() == 1 && q.pending() <= TxQueue::kCapacity - TxQueue::kReplyReserve,
        "tx: log lines drop once the queue is full up to the reply reserve");
  q.put("reply\n", 6, TxClass::Reply);
  check(q.dropped() == 1, "tx: a reply still fits in the reserve");
  const std::string seen = drainQueue(q);
  check(seen.size() >= 6 && seen.compare(seen.size() - 6, 6, "reply\n") == 0, "tx: the reply is queued behind the log lines");

  const std::string reply(500, 'r');
  while (q.dropped() == 1) {
    q.put(reply.data(), reply.size(), TxClass::Reply);
    q.put("\n", 1, TxClass::Reply);
  }
  check(q.pending() <= TxQueue::kCapacity, "tx: replies drop only when the whole queue is full");
}

// discard() empties the finished lines; the line being written keeps its bytes and ends whole.
void checkTxDiscard() {
  TxQueue q;
  q.put("lost\n", 5, TxClass::Reply);
  q.discard();
  check(q.pending() == 0 && q.dropped() == 0, "tx: discard empties the queue without counting");

  q.put("op", 2, TxClass::Log);
  q.discard();
  q.put("en\n", 3, TxClass::Log);
  check(drainQueue(q) == "open\n", "tx: a line open at discard time is not cut into a fragment");
}

}  // namespace

void checkTx() {
  checkTxQueue();
  checkTxReplyReserve();
  checkTxDiscard();
}
