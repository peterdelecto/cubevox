// Layout record, schema v2 (spec 2026-10-12-layout-record-design.md). Pure codec: no flash access.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <array>

#include "core/crc32.h"
#include "core/flash_io.h"
#include "core/layout.h"
#include "core/settings.h"
#include "core/slots.h"

constexpr size_t kRecordBytes = 256;
constexpr size_t kCardFieldReservedBytes = 8;
constexpr size_t kRecordCardFieldBytes = kCardIdBytes + kCardFieldReservedBytes;  // wire id, then reserved

// Byte offsets of the fields, in file order.
constexpr size_t kOffMagic = 0;
constexpr size_t kOffSchema = 4;
constexpr size_t kOffSequence = 8;
constexpr size_t kOffName = 12;
constexpr size_t kOffSlots = kOffName + kLayoutNameBytes;
constexpr size_t kOffSettings = kOffSlots + kSlotCount * kRecordCardFieldBytes;
constexpr size_t kSettingsBytes = 16;  // output level, input jack, polish comp, polish eq, top switch, 11 reserved
constexpr size_t kOffCrc = kOffSettings + kSettingsBytes;

// Byte offsets of the settings, in file order. A v1 record has the first two.
constexpr size_t kOffOutputLevel = kOffSettings;
constexpr size_t kOffInputJack = kOffSettings + 1;
constexpr size_t kOffPolishComp = kOffSettings + 2;
constexpr size_t kOffPolishEq = kOffSettings + 3;
constexpr size_t kOffTopSwitch = kOffSettings + 4;

constexpr uint16_t kRecordSchema = 2;
constexpr uint16_t kRecordSchemaV1 = 1;  // still read; the v2 settings take their defaults

static_assert(kOffCrc + 4 == kRecordBytes, "record fields must add up to 256 bytes");
static_assert(kRecordBytes % kFlashWordBytes == 0, "record must be a whole number of flash words");

struct RecordData {
  Layout layout = defaultLayout();
  Settings settings;
  char name[kLayoutNameBytes + 1] = {};  // NUL-terminated copy
  uint32_t sequence = 0;
  bool unknownCard = false;  // set by decodeRecord when a card id matched no card and became Empty
};

using RecordBytes = std::array<uint8_t, kRecordBytes>;

RecordBytes encodeRecord(const RecordData& rec);

// False when magic, schema, CRC or a settings byte is wrong. A v1 record decodes with the default
// polish settings; encodeRecord always writes v2. Unknown card ids decode to Card::Empty and set
// `out.unknownCard`.
bool decodeRecord(const uint8_t* bytes, RecordData& out);

// One record-sized entry of a sector's append log.
enum class EntryState : uint8_t { Blank, Valid, Torn };

// Blank is all 0xFF (erased). Valid decodes. Anything else is a record cut off mid-write.
EntryState classifyRecordEntry(const uint8_t* bytes);

struct RecordFound {
  RecordData rec;
  int sector = 0;
  int entry = 0;
};

// The valid record with the highest sequence across the sector views, each `sectorBytes` long.
bool newestRecord(const uint8_t* const* sectors, int sectorCount, size_t sectorBytes, RecordFound& out);
