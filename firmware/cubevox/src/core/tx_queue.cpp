#include "core/tx_queue.h"

void TxQueue::putByte(char c) {
  if (!failed_ && tail_ - head_ >= kCapacity) {
    tail_ = commit_;  // roll the open line back
    failed_ = true;
  }
  if (failed_) {
    if (c == '\n') {
      failed_ = false;
      ++dropped_;
    }
    return;
  }
  buf_[tail_ % kCapacity] = c;
  ++tail_;
  if (c == '\n') commit_ = tail_;
}

void TxQueue::put(const char* text, size_t len) {
  for (size_t i = 0; i < len; ++i) putByte(text[i]);
}

size_t TxQueue::peek(const char*& run) const {
  const size_t start = head_ % kCapacity;
  size_t n = commit_ - head_;
  if (n > kCapacity - start) n = kCapacity - start;
  run = buf_ + start;
  return n;
}

void TxQueue::consume(size_t len) { head_ += len; }

void TxQueue::discard() {
  head_ = tail_;
  commit_ = tail_;
  failed_ = false;
}
