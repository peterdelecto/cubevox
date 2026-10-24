#include "core/record.h"

#include <string.h>

#include "core/le_bytes.h"
#include "core/registry.h"

namespace {

constexpr std::array<uint8_t, 4> kMagic = {'C', 'V', 'L', 'R'};

// An id that matches no card reads as Empty, and `known` says so.
Card cardFromField(const uint8_t* field, bool& known) {
  std::array<char, kCardIdBytes + 1> id{};
  memcpy(id.data(), field, kCardIdBytes);
  Card card = Card::Empty;
  known = cardFromId(id.data(), card);
  return known ? card : Card::Empty;
}

// Reads the settings bytes. A v1 record carries only the first two, so the polish settings keep
// the defaults already in `s`. False when a byte is out of range.
bool decodeSettings(const uint8_t* b, uint16_t schema, Settings& s) {
  const uint8_t output = b[kOffOutputLevel];
  const uint8_t jack = b[kOffInputJack];
  if (output > static_cast<uint8_t>(OutputLevel::Instrument)) return false;
  if (jack > static_cast<uint8_t>(InputJack::Pedal)) return false;
  s.outputLevel = static_cast<OutputLevel>(output);
  s.inputJack = static_cast<InputJack>(jack);
  if (schema == kRecordSchemaV1) return true;

  const uint8_t comp = b[kOffPolishComp];
  const uint8_t eq = b[kOffPolishEq];
  const uint8_t top = b[kOffTopSwitch];
  if (comp > 1 || eq > 1) return false;
  if (top > static_cast<uint8_t>(TopSwitch::Bypass)) return false;
  s.polishComp = comp != 0;
  s.polishEq = eq != 0;
  s.topSwitch = static_cast<TopSwitch>(top);
  return true;
}

}  // namespace

RecordBytes encodeRecord(const RecordData& rec) {
  RecordBytes b{};  // reserved bytes stay zero
  memcpy(b.data() + kOffMagic, kMagic.data(), kMagic.size());
  put16(b.data() + kOffSchema, kRecordSchema);
  put32(b.data() + kOffSequence, rec.sequence);
  memcpy(b.data() + kOffName, rec.name, kLayoutNameBytes);  // name[] is zero-padded
  for (int s = 0; s < kSlotCount; ++s) {
    const char* id = cardDesc(rec.layout.card[s]).id;
    strncpy(reinterpret_cast<char*>(b.data() + kOffSlots + s * kRecordCardFieldBytes), id, kCardIdBytes);
  }
  b[kOffOutputLevel] = static_cast<uint8_t>(rec.settings.outputLevel);
  b[kOffInputJack] = static_cast<uint8_t>(rec.settings.inputJack);
  b[kOffPolishComp] = rec.settings.polishComp ? 1 : 0;
  b[kOffPolishEq] = rec.settings.polishEq ? 1 : 0;
  b[kOffTopSwitch] = static_cast<uint8_t>(rec.settings.topSwitch);
  put32(b.data() + kOffCrc, crc32(b.data(), kOffCrc));
  return b;
}

bool decodeRecord(const uint8_t* b, RecordData& out) {
  if (memcmp(b + kOffMagic, kMagic.data(), kMagic.size()) != 0) return false;
  const uint16_t schema = get16(b + kOffSchema);
  if (schema != kRecordSchema && schema != kRecordSchemaV1) return false;
  if (get32(b + kOffCrc) != crc32(b, kOffCrc)) return false;

  RecordData rec;
  if (!decodeSettings(b, schema, rec.settings)) return false;
  rec.sequence = get32(b + kOffSequence);
  memcpy(rec.name, b + kOffName, kLayoutNameBytes);
  for (int s = 0; s < kSlotCount; ++s) {
    bool known = false;
    rec.layout.card[s] = cardFromField(b + kOffSlots + s * kRecordCardFieldBytes, known);
    if (!known) rec.unknownCard = true;
  }
  out = rec;
  return true;
}

EntryState classifyRecordEntry(const uint8_t* bytes) {
  RecordData scratch;
  if (decodeRecord(bytes, scratch)) return EntryState::Valid;
  return isErased(bytes, kRecordBytes) ? EntryState::Blank : EntryState::Torn;
}

bool newestRecord(const uint8_t* const* sectors, int sectorCount, size_t sectorBytes, RecordFound& out) {
  bool found = false;
  for (int s = 0; s < sectorCount; ++s) {
    for (size_t off = 0; off + kRecordBytes <= sectorBytes; off += kRecordBytes) {
      RecordData rec;
      if (!decodeRecord(sectors[s] + off, rec)) continue;
      if (found && rec.sequence <= out.rec.sequence) continue;
      out.rec = rec;
      out.sector = s;
      out.entry = static_cast<int>(off / kRecordBytes);
      found = true;
    }
  }
  return found;
}
