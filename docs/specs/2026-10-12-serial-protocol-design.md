# Serial protocol and live layout apply — design

Roadmap step 3 of `2026-10-08-update-and-slots-design.md` (Apply item 4, Serial item 6).
JSON handling follows `2026-10-08-firmware-foundation-design.md` (Decisions, "JSON"): hand-written,
flat requests, replies built with `snprintf`, no ArduinoJson.

## Transport
1. USB CDC, one JSON object per line, `\n` terminated. Requests carry `"id"` (integer) and `"cmd"`;
   the reply echoes `id`.
2. The port also carries the boot report and the single-letter bench commands. A byte `{` starts
   a JSON line, which runs to `\n`; any other byte outside a line is a bench letter. The app reads
   only lines that start with `{`.
3. Lines longer than 768 bytes are dropped with an error reply (`id` 0).
4. Output never blocks `loop()`. Replies, the boot report and bench prints go through a bounded
   transmit queue drained with non-blocking writes each pass, so a host that stops reading can't
   stall the loop into the watchdog. Overflow drops whole lines and counts them.
5. The layout `name` is 1 to 31 printable ASCII characters; anything else is rejected. Replies
   escape every string they carry.

## Commands
| cmd | Request fields | Reply |
|---|---|---|
| `hello` | — | `proto` 1, `fw` version, `cards` (registry: id, name, group, cost avg/worst, measured, knobs: name, kind, positions, min, max, unit, curve), `layout` |
| `get_layout` | — | `layout`: `name`, `slots` (8 card ids) |
| `set_layout` | `name`, `slots` (8 card ids) | `ok`, or an error |
| `status` | — | CPU load % mean and worst since the last status (own baseline; never resets the shared peaks), `late`, `muted`, record `seq` and write errors, serial lines dropped |
| `enter_dfu` | — | `ok`, sent and flushed before the box mutes and reboots |

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
4. Controls re-seed knob steps for the new cards (already in place); the menu's chain view reads
   the new layout.

## Modules
| Tier | Module | Owns |
|---|---|---|
| core | `protocol` | line framing state, request parse, reply formatting into a writer interface |
| core | `layout_fade` | fade-out, swap, fade-in per block |
| board | `serial_link` | reads CDC bytes, splits JSON lines from bench letters, writes replies |
| sketch | | dispatches commands to layout, record, audio stats, mute and DFU |

## Verification
1. Host tests: each command parses; malformed JSON, missing fields, wrong types, overlong lines and
   unknown commands give error replies; `hello` output parses as JSON (checked by a small reader
   in the test) and lists every card; fade gain is monotonic, reaches zero before the swap and
   returns to one. A fuzz loop feeds random bytes and truncated JSON through the parser under the
   sanitizers.
2. `tools/protocol_check.py`: talks to the box over the port; runs hello, get, set valid, set
   invalid (unknown id, two reverbs, duplicate), status, and checks the layout survives a reset.
   Run on the WeAct bench (chip checks only); audio stays at 0 late through every apply.
