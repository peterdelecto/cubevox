# Serial protocol and live layout apply — design

Roadmap step 3 of `2026-10-08-update-and-slots-design.md` (Apply item 4, Serial item 6).
JSON handling follows `2026-10-08-firmware-foundation-design.md` (Decisions, "JSON"): hand-written,
flat requests, replies built with `snprintf`, no ArduinoJson.

## Transport
1. USB CDC, one JSON object per line, `\n` terminated. Requests carry `"id"` (integer) and `"cmd"`;
   the reply echoes `id`.
2. The port also carries the boot report and diagnostic log lines. A byte `{` starts a JSON line,
   which runs to `\n`. Every other byte outside a line is ignored and counted (`stray_bytes` in
   `status`), so no single byte can act. The app reads only lines that start with `{`.
3. Lines longer than 768 bytes are dropped with an error reply (`id` 0).
4. Output never blocks `loop()`. Replies, the boot report and bench prints go through a bounded
   transmit queue drained with non-blocking writes each pass, so a host that stops reading can't
   stall the loop into the watchdog. Overflow drops whole lines and counts them.
5. The layout `name` is 1 to 31 printable ASCII characters; anything else is rejected. Replies
   escape every string they carry.

## Commands
| cmd | Request fields | Reply |
|---|---|---|
| `hello` | — | `proto` 1, `fw` version, `serial` (24 uppercase hex characters of the MCU's 96-bit unique id, the text the update drive serves as its USB serial), `build` (`release` or `bench`), `bench_ops` (ops this build accepts), `cards` (registry: id, name, group, cost avg/worst, measured, knobs: name, kind, positions, min, max, unit, curve), `layout`, `settings` (the box always sends it; the host tool that has no board leaves it out) |
| `get_layout` | — | `layout`: `name`, `slots` (8 card ids); `settings` |
| `set_layout` | `name`, `slots` (8 card ids), optional `settings` | `ok`, or an error |
| `status` | — | CPU load % mean and worst since the last status (own baseline; never resets the shared peaks), `late` (blocks past their budget or written after the DMA began reading their half), `muted`, record `seq` and write errors, serial lines dropped, `stray_bytes`, `boot_fails` (earlier starts that never reported healthy, read at boot), the audio faults since boot (`overruns`, `sai_errors`, `restarts`, `nan_blocks`, `stalled` for the stall watch's latch, `audio_latched` for a fault past the restart budget), `power_trips` (power-fail mutes), `reply_overflows` (reply pieces lost to the format buffer), `update_reason` (why the bootloader last stayed in update mode: `none`, `MenuHeld`, `AppRequest`, `ImageInvalid`, `ImageUnreadable` or `BootFailed`) |
| `enter_dfu` | — | `ok`, sent and flushed before the box mutes and reboots into the UF2 update drive (the `CUBEVOX` volume, spec `2026-10-24-field-update-design.md`) |
| `bench` | `op`, plus `slot` (1 to 8) for `slot_off` | `ok` after the op runs, or an error; diagnostic text goes to the log lines |

## Settings object
Owner 2026-10-24, `2026-10-24-polish-design.md`. The settings that live in the layout record's
settings block and belong to the polish stages travel with the layout, as one object next to it.

| Field | Values | Default on a fresh box |
|---|---|---|
| `polish_comp` | `true`, `false` | `true` |
| `polish_eq` | `true`, `false` | `true` |
| `top_switch` | `"mute"`, `"bypass"` | `"mute"` |

1. Replies always carry all three. A `set_layout` request may leave out the object or any field in
   it, and a missing field leaves the box's current value. Only the fields a request carries change,
   so a request without `settings` changes none of them.
2. A wrong type or an unknown word is an error that names the field, for example
   `top_switch must be "mute" or "bypass"`. Unknown keys inside the object are skipped.
3. The output level and input jack settings stay off the wire; the menu owns them.
4. The box applies the settings with the layout and saves the record through the usual debounce.

## Bench operations
Owner 2026-10-12: a performance box must never act on an unframed byte, and Adam's release image
carries no drill it doesn't need. This realises item 5 of `2026-10-08-effect-registry-design.md`
(bench behind `CUBEVOX_BENCH`).

| Class | Ops | Builds |
|---|---|---|
| Query | `report`, `stats`, `record_status` | all |
| Bench | `slot_off`, `slots_on`, `pin_knobs`, `chasm`, `clip`, `reset_peaks`, `stop_audio`, `fault`, `spin`, `record_save`, `record_erase`, `panel_test`, `panel_summary`, `panel_dump`, `panel_jack`, `panel_mute`, `panel_oled` | `CUBEVOX_BENCH` only |

1. One table in `core/bench_ops` holds each op's wire name and class. `hello` lists the ops the
   build accepts, from that table.
2. A bench op sent to a release image replies `op not in this build: <op>`. Its board code sits
   under `#if CUBEVOX_BENCH`, so the release image does not contain it.
3. DFU stays `enter_dfu` in every build, because the app needs it.
4. `./build.sh` makes the release image and `BENCH=1 ./build.sh` the bench image. The boot report
   prints `[boot] build release|bench`.
5. Host tools map the familiar letters to ops in one place (`firmware/bench_ops.py`), so
   `term.py -s p` still works and sends JSON.
6. First-article bring-up flashes the bench image, runs every check, then flashes the release image
   and confirms `hello` reports `release`.

Errors reply `{"id":N,"ok":false,"error":"<reason>"}`. Reasons are short and readable on the page,
for example `unknown card "foo" in slot 3`, `two reverb cards`, `same card twice: slapback`,
`over CPU budget (worst 97 %)`. A `set_layout` with an unknown id is an error; only the record
load maps unknown ids to `empty`.

## Apply
1. `set_layout` validates with `layoutValid` (with a reason), then sets the sketch's layout,
   publishes, requests a record save, and replies.
2. The audio interrupt holds its own copy of the active snapshot. A published snapshot with the
   same layout is adopted at once (knob moves). One with a different layout starts a fade: the
   held snapshot keeps running while the output ramps to zero over 50 ms; at zero the interrupt
   adopts the new snapshot, resets the engines whose slot changed card, and ramps back over 50 ms.
   A second layout arriving mid-fade is taken at the next zero point.
   Clearing a large engine (reverb, pitch) inside the interrupt costs block time. The bench check
   measures the worst block through a swap. If it goes late, the clears are spread over the
   silent blocks before the fade-in.
3. The ramp is a pure core sequencer (`core/layout_fade`): per block it gives the gain and whether
   to swap. Host-tested.
4. Controls re-seed knob steps for the new cards (already in place). No menu row opens the chain
   view (`2026-10-24-polish-design.md`).

## Modules
| Tier | Module | Owns |
|---|---|---|
| core | `protocol` | line framing state, request parse, reply formatting into a writer interface |
| core | `layout_fade` | fade-out, swap, fade-in per block |
| board | `serial_link` | reads CDC bytes, cuts JSON lines and ignores every other byte (counted as stray), writes replies |
| sketch | | dispatches commands to layout, record, audio stats, mute and DFU |

## Verification
1. Host tests: each command parses; malformed JSON, missing fields, wrong types, overlong lines and
   unknown commands give error replies; `hello` output parses as JSON (checked by a small reader
   in the test) and lists every card; fade gain is monotonic, reaches zero before the swap and
   returns to one. A fuzz loop feeds random bytes and truncated JSON through the parser under the
   sanitizers.
2. `tools/protocol_check.py`: talks to the box over the port; runs hello, get, set valid, set
   invalid (unknown id, two reverbs, duplicate), status, and checks the layout survives a reset.
   Run on the test board (chip checks only); audio stays at 0 late through every apply.
