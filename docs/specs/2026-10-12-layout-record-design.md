# Layout record in flash bank 2 — design

Roadmap step 2 of `2026-10-08-update-and-slots-design.md`. The record schema is fixed there and in
`2026-10-08-firmware-foundation-design.md` (Decisions, "Record schema v1"). This spec settles how
the record is written and recovered on the H743.

## Hardware facts that shape it
1. Bank 2 sectors are 128 KB. A sector erase takes about 1 to 2 s (up to 4 s), longer than the
   2 s watchdog, so the erase never blocks `loop()`.
2. Flash programs in 32-byte flash words with ECC. A word cut off mid-program can read back as a
   double ECC error, which is a bus fault. Bank 2 survives DFU, so a fault on that word would
   repeat on every boot.
3. Bank 1 and bank 2 are separate, so the code keeps running from bank 1 while bank 2 erases.

## Format (schema v1)
| Field | Bytes |
|---|---|
| magic `CVLR` | 4 |
| schema | 2 |
| reserved | 2 |
| sequence | 4 |
| name | 32 |
| 8 slots × (card wire id[16], reserved[8]) | 192 |
| settings: output level, input jack, 14 reserved | 16 |
| CRC32 over everything before it | 4 |

256 bytes, eight flash words. A card id the firmware does not know becomes `empty`.

## Format (schema v2)
Owner 2026-10-24, `2026-10-24-polish-design.md`. Only the settings block changes. Every offset and
the 256-byte size stay, and the three new bytes come out of the reserved ones.

| Settings byte | Field | Values | Default |
|---|---|---|---|
| 0 | output level | 0 LINE, 1 MIC | LINE |
| 1 | input jack | 0 MIC, 1 LINE | MIC |
| 2 | polish comp | 0 off, 1 on | on |
| 3 | polish EQ | 0 off, 1 on | on |
| 4 | top switch | 0 MUTE, 1 BYPASS | MUTE |
| 5 to 15 | reserved, zero | | |

1. A v1 record still decodes. Its bytes 2 to 4 were reserved, so they are ignored and the three
   new settings take their defaults, not the zeros.
2. Every save writes v2, so a v1 record is replaced by the next save.
3. A v1 firmware refuses a v2 record on the schema field and falls back to the default layout, as
   for any unknown schema.
4. A settings byte out of range makes the whole record invalid, as in v1.

## Writing
1. Each sector is a log of 256-byte records. A save appends the next record (sequence + 1) to the
   first blank slot of the active sector, so the previous record is never touched.
2. When the active sector is full, the save goes to the other sector after a non-blocking erase.
   A sector holds 512 records, so erases are rare (flash endurance is 10 k cycles per sector).
3. A save starts 500 ms after the last change, so a run of menu clicks writes once, and at once when the menu closes.
   A save whose encoded record matches the newest one in every byte but the sequence and the CRC is not written;
   the bench `record_save` op writes anyway.
4. Erase and program run as a state machine polled from `loop()`. An erase is started and polled across passes. A program
   call writes one flash word, which takes tens of microseconds, and waits for that word at most 5 ms before it reports an
   error. No other call waits on the flash.

## Reading at boot
1. Scan both sectors; the valid record (magic, schema 1 or 2, CRC) with the highest sequence wins.
2. No valid record gives the default layout and default settings.
3. ECC recovery: the fault handler also stores bank 2's double-ECC flag and failing address
   (`FLASH->SR2`, `FLASH->ECC_FA2`), because BFAR is not always valid for a flash ECC error. If
   the last reset was a fault at a bank 2 address, the boot recovers that sector before scanning.
   The records that end before the faulted word read safely, since the scan that faulted read them
   first. When the newest of them is newer than everything in the other sector, it is copied to
   entry 0 there (that sector erased first unless blank), and then the faulted sector is erased.
   The other sector is read only when there is a record to carry, and the copy sits alone in its
   sector, so a power cut during the copy cannot leave two recoveries faulting on each other's
   sector. A sector that will not erase stays unread for that start. One bad word costs only older
   records, never the newest, and never a boot loop.

## Modules
| Tier | Module | Owns |
|---|---|---|
| core | `record` | encode, decode, CRC32, newest-valid selection over a byte view of the sectors |
| core | `record_store` | append, sector switch and erase sequencing as a pure state machine over a flash interface |
| board | `flash_bank2` | erase start, erase poll, program one flash word, read; cache maintenance |

## Out of scope here
Live layout apply with the 50 ms fade comes with the serial protocol (roadmap step 3), the
first path that changes the layout at runtime.

## Verification
1. Host tests: round trip, CRC failure, unknown id to `empty`, newest wins across both sectors,
   torn last record ignored, sector full switches and erases, interrupted erase recovers, a torn tail
   after N records boots on record N, an unchanged state writes nothing.
2. Test board (chip checks only): bench commands save, report, and erase bank 2; a saved setting
   survives a reset and a DFU reflash; a save during audio leaves block timing at 0 late.
