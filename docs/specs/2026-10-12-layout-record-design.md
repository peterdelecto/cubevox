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

## Writing
1. Each sector is a log of 256-byte records. A save appends the next record (sequence + 1) to the
   first blank slot of the active sector, so the previous record is never touched.
2. When the active sector is full, the save goes to the other sector after a non-blocking erase.
   A sector holds 512 records, so erases are rare (flash endurance is 10 k cycles per sector).
3. A save starts 2 s after the last settings change, so turning through the menu writes once.
4. Erase and program run as a state machine polled from `loop()`. An erase is started and polled across passes. A program
   call writes one flash word, which takes tens of microseconds, and waits for that word at most 5 ms before it reports an
   error. No other call waits on the flash.

## Reading at boot
1. Scan both sectors; the valid record (magic, schema, CRC) with the highest sequence wins.
2. No valid record gives the default layout and default settings.
3. ECC recovery: the fault handler also stores bank 2's double-ECC flag and failing address
   (`FLASH->SR2`, `FLASH->ECC_FA2`), because BFAR is not always valid for a flash ECC error. If
   the last reset was a fault at a bank 2 address, the boot erases that sector before scanning.
   One bad word costs that sector's history, never a boot loop.

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
   torn last record ignored, sector full switches and erases, interrupted erase recovers.
2. Test board (chip checks only): bench commands save, report, and erase bank 2; a saved setting
   survives a reset and a DFU reflash; a save during audio leaves block timing at 0 late.
