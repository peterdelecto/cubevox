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
| 3 | retired, written 0 and ignored on read (POLISH EQ's ON/OFF until VOX EQ replaced it, 2026-11-05, `2026-11-05-vox-eq-design.md`) | | |
| 4 | top switch | 0 MUTE, 1 BYPASS | MUTE |
| 5 | debug | 0 off, 1 on (owner 2026-10-30, `2026-10-24-polish-design.md` menu row 11) | off |
| 6 and 7 | retired, written 0 and ignored on read (the HPF and LPF bytes until POLISH EQ took the filters, 2026-10-31) | | |
| 8 | brightness | 0 HIGH (a record written before the byte existed), 1 LOW, 2 MID, 3 HIGH, 4 MAX; above 4 the record is invalid (owner 2026-11-02, `2026-10-24-polish-design.md` menu row 10) | HIGH, written as 3 |
| 9 | VOX EQ | 0 OFF, 1 ON, 2 ON+CUTS; above 2 the record is invalid; 0 in every record written before the byte existed (owner 2026-11-05, `2026-11-05-vox-eq-design.md`) | OFF |
| 10 to 15 | reserved, zero | | |

Bytes 5 and 8 came out of the reserved bytes on 2026-10-30 and 2026-11-02 and the schema stays 2:
no earlier decoder read either byte, every record written before holds 0 there, and 0 reads as the
default.

1. A v1 record still decodes. Its bytes 2 to 4 were reserved, so they are ignored and the three
   new settings take their defaults, not the zeros.
2. Every save writes v2, so a v1 record is replaced by the next save.
3. A v1 firmware refuses a v2 record on the schema field and falls back to the default layout, as
   for any unknown schema.
4. A settings byte out of range makes the whole record invalid, as in v1.
5. Every schema, a later one included, keeps the magic, the sequence at byte 8 and the CRC over
   bytes 0 to 251, so a build reads the sequence of a record it cannot decode.

## Writing
1. Each sector is a log of 256-byte records. A save appends the next record to the first blank slot
   of the active sector, so the previous record is never touched. Its sequence is one past the
   highest sequence in flash, counting every entry whose magic and CRC are good, so a record a
   later build wrote never ties or beats it after a downgrade.
2. When the active sector is full, the save goes to the other sector after a non-blocking erase.
   A sector holds 512 records, so erases are rare (flash endurance is 10 k cycles per sector).
   The move never erases the sector that holds the newest record.
3. A sector that holds a torn entry (neither blank nor a record this build reads) or a cut erase
   takes no more appends, and the next save moves the log. A word cut off mid-program therefore
   only ever sits after the newest record of its sector, never below a newer one. A failed program
   stops the appends to its sector the same way.
4. A save that fails is tried once more 500 ms later, in the other sector when flash holds a
   record. A second failure is left alone until the next change.
5. While the power-fail trip holds (`board/power_sense`), neither the record nor the diagnostic log
   starts an erase or a program word. A save mid-program stops between words and goes on if the
   rail recovers.
6. A save starts 500 ms after the last change, so a run of menu clicks writes once, and at once when the menu closes.
   A save whose encoded record matches the newest one in every byte but the sequence and the CRC is not written;
   the bench `record_save` op writes anyway.
7. Erase and program run as a state machine polled from `loop()`. An erase is started and polled across passes. A program
   call writes one flash word, which takes tens of microseconds, and waits for that word at most 5 ms before it reports an
   error. Only the boot's ECC recovery waits on whole erases.

## Reading at boot
1. Scan both sectors up to their read limits (item 4); the valid record (magic, schema 1 or 2, CRC)
   with the highest sequence wins. On equal sequences, as a cut erase after a carried record
   leaves them, the copy in a sector without a cut erase wins and the log goes on there.
2. No valid record gives the default layout and default settings. A record whose layout fails a
   check gives the default layout with the record's settings, and one save rewrites it.
3. Newer records that earlier firmware appended past a torn entry load, and the newest is saved
   again in the other sector at once, so the torn word faulting later costs nothing.
4. Read limits: each record sector has a limit, the bytes from its start a start may read. They
   live in `.cv_noinit` after the fault record (`board/fault`), sealed with a magic and a CRC, so
   they outlive a fault reset. A power-on (first reset flag power-on) or a broken seal sets both to
   the whole sector. A double ECC error lowers its sector's limit to the start of the entry that
   holds the word, and a finished erase of the sector lifts it again. No start reads past a limit,
   so a sector faults at most once per power-on, and two recoveries can never fault on each other's
   sector in turn.
5. ECC recovery: the fault handler also stores bank 2's double-ECC flag and failing address
   (`FLASH->SR2`, `FLASH->ECC_FA2`), because BFAR is not always valid for a flash ECC error. If
   the last reset was a fault at a bank 2 address, the boot recovers that sector before scanning.
   It lowers the sector's limit to the faulted entry; the records below it read safely, since the
   scan that faulted read them first. When the newest of them is newer than everything readable in
   the other sector, it is copied to entry 0 there (that sector erased first unless blank), and
   then the faulted sector is erased. The other sector is read only when there is a record to
   carry. A failed copy keeps the faulted sector, read up to the fault, and closes the other one
   until its next erase. A sector that will not erase stays unread.
6. Failed starts: the bootloader counts every start that does not run healthy for 5 s and stays in
   update mode after three (`CV_BOOT_FAIL_LIMIT`), so each faulting start of a recovery uses one. A
   start that recovers from the last run's own bank 2 ECC fault and learns a bad word from it (a
   record sector's limit came down, or the log's sector erased) takes one off the count. Every
   learned word lowers a limit for good, so the give-back ends.
7. The result, which the host test `core_record_cut.cpp` checks with a flash that faults on a bad
   word and the bootloader's own decision: a power cut at any flash step of a save, an erase or a
   recovery, and at any step of the recovery after that, ends in a start that loads a valid layout
   at least as new as the newest record written whole, and never in update mode. A bad word costs
   only records older than the newest.

## Modules
| Tier | Module | Owns |
|---|---|---|
| core | `record` | encode, decode, CRC32, newest-valid selection over a byte view of the sectors |
| core | `record_store` | append, sector switch and erase sequencing as a pure state machine over a flash interface; read limits, ECC recovery, boot load |
| board | `flash_bank2` | erase start, erase poll, program one flash word, read; cache maintenance |
| board | `fault` | the read limits' storage in `.cv_noinit` |
| board | `record_service` | the boot path (limits, recovery, give-back, load), the poll, the reports |

## Out of scope here
Live layout apply with the 50 ms fade comes with the serial protocol (roadmap step 3), the
first path that changes the layout at runtime.

## Verification
1. Host tests: round trip, CRC failure, unknown id to `empty`, newest wins across both sectors,
   torn last record ignored, sector full switches and erases, interrupted erase recovers, a torn tail
   after N records boots on record N, an unchanged state writes nothing, a torn entry moves the log,
   a failed save is retried, the sequence floor, the read limits, the boot load
   (`core_record.cpp`), and the power-cut sweep of Reading at boot item 7 (`core_record_cut.cpp`).
2. Test board (chip checks only): bench commands save, report, and erase bank 2; a saved setting
   survives a reset and a DFU reflash; a save during audio leaves block timing at 0 late.
