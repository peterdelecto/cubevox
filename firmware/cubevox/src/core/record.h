// Layout record, schema v1 (spec 2026-10-12-layout-record-design.md). Pure codec: no flash access.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <array>

#include "core/layout.h"
#include "core/settings.h"

constexpr size_t kRecordBytes = 256;
constexpr size_t kRecordNameBytes = 32;
constexpr uint16_t kRecordSchema = 1;
constexpr size_t kCardIdBytes = 16;
constexpr size_t kSlotBytes = 24;  // wire id[16] and 8 reserved bytes

// Byte offsets of the fields, in file order.
constexpr size_t kOffMagic = 0;
constexpr size_t kOffSchema = 4;
constexpr size_t kOffSequence = 8;
constexpr size_t kOffName = 12;
constexpr size_t kOffSlots = kOffName + kRecordNameBytes;
constexpr size_t kOffSettings = kOffSlots + kSlotCount * kSlotBytes;
constexpr size_t kSettingsBytes = 16;  // output level, input jack, 14 reserved
constexpr size_t kOffCrc = kOffSettings + kSettingsBytes;

// A record is whole flash words (32 bytes) so each one starts on a word boundary.
static_assert(kOffCrc + 4 == kRecordBytes, "record fields must add up to 256 bytes");
static_assert(kRecordBytes % 32 == 0, "record must be a multiple of the 32-byte flash word");

struct RecordData {
  Layout layout = defaultLayout();
  Settings settings;
  char name[kRecordNameBytes + 1] = {};  // NUL-terminated copy
  uint32_t sequence = 0;
};

using RecordBytes = std::array<uint8_t, kRecordBytes>;

// Standard CRC-32 (reflected 0xEDB88320, init and final xor 0xFFFFFFFF).
uint32_t crc32(const uint8_t* data, size_t len);

RecordBytes encodeRecord(const RecordData& rec);

// False when magic, schema, CRC or a settings byte is wrong. Unknown card ids decode to Card::Empty.
bool decodeRecord(const uint8_t* bytes, RecordData& out);

enum class SlotState : uint8_t { Blank, Valid, Torn };

// Blank is all 0xFF (erased). Valid decodes. Anything else is a record cut off mid-write.
SlotState classifyRecordSlot(const uint8_t* bytes);

struct RecordFound {
  RecordData rec;
  int sector = 0;
  int slot = 0;
};

// The valid record with the highest sequence across the sector views, each `sectorBytes` long.
bool newestRecord(const uint8_t* const* sectors, int sectorCount, size_t sectorBytes, RecordFound& out);
