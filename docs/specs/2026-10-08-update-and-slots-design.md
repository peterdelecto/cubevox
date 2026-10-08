# Firmware update and slot layout design (owner, 2026-10-08)

## Plan

Adam customises the box from his Mac. One curl line installs `cubevox.app`. He double-clicks it, his browser opens a Web Awesome page, and he drags effect cards from a library into the eight slots. The page pushes the layout to the box over the USB-C cable and the box applies it live. The same page updates the firmware and the app. The box stores one live layout in flash bank 2. The OLED shows the layout and never edits it.

The firmware carries every effect. A firmware update only matters when a new card joins the library or an effect changes. Slot hardware is fixed and identical (two pots and a bypass toggle per slot), which is what makes any card fit any slot.

### Pieces

| Piece | Where | What it does |
|---|---|---|
| Page | `app/web/`, embedded in the app | Layout editor, saved layouts, update screen. Web Awesome components. |
| App | `app/`, Go, one binary in `cubevox.app` | Serves the page on 127.0.0.1, relays JSON to the box over USB CDC, runs dfu-util, keeps saved layouts, checks GitHub Releases. |
| Firmware | `firmware/cubevox/` | Effect registry with stable ids and CPU costs, layout record in bank 2, serial command set, live apply with a 50 ms fade. |
| Release | GitHub Releases on `peterdelecto/cubevox` | One release per tag `vX.Y.Z` holding `cubevox.bin`, `cubevox.app.tar.gz`, `install.sh`. Built by a GitHub Action. |

### Install and launch

`curl -fsSL https://github.com/peterdelecto/cubevox/releases/latest/download/install.sh | sh` downloads the latest `cubevox.app` into `/Applications` and replaces an older copy. The same line updates the app; the Update screen can also do it by itself. curl does not set the quarantine flag, so Gatekeeper does not block the app. dfu-util ships inside the bundle as a static binary, so Homebrew is not needed.

Double-click starts the server on a free local port and opens the default browser. The app quits 60 seconds after the last page closes. `cubevox.app/Contents/MacOS/cubevox --fake` runs with a fake box so the page can be built and tested with no hardware.

### Page

1. **Layout.** Eight slot cards in one row, left to right as on the face. A library panel lists every card with its two knob names and CPU cost. Click or drag a card into a slot. A card already placed is greyed in the library. Empty is a card. A CPU meter sums the placed cards; above 85 % it turns red and Push is disabled. Push sends the layout; the box confirms and the page shows the live layout it read back.
2. **Layouts.** Named layouts kept on the Mac in `~/Library/Application Support/cubevox/layouts/*.json`. Save current, push, duplicate, rename, delete, export and import. The box holds only the live one.
3. **Update.** Firmware version on the box, app version, latest release on GitHub, release notes. One Update button updates whichever is behind. For firmware the app backs the live layout up to the Mac, tells the box to enter DFU, flashes with dfu-util, waits for the box to enumerate again and reads its version back.
4. **Status bar.** Connected or not, box firmware version, live CPU from the box.

### Firmware

1. **Effect registry.** Each card has a stable string id that never changes once shipped, a display name, two knob descriptors (name, range, step rule such as KEY snapping), and a CPU cost. Ids for v1: `empty`, `input_gate`, `autotune`, `octave`, `unison`, `slapback`, `distortion`, `gate`, `reverb_spring`, `reverb_chasm`. Each reverb engine is its own card; the reverb engine menu item goes away. Harmony has no card and the old Spring engine is removed (owner 2026-10-08). Groups (one reverb per layout, pitch cards as one stage), descriptor fields, the estimate flag and bench mode are in `2026-10-08-effect-registry-design.md`.
2. **CPU cost.** Percent of one audio frame at peak, measured on the box by a bench mode that runs each card alone, written into `firmware/cubevox/costs.h` per release. The box also measures live frame time and reports it. The box rejects a layout whose summed cost exceeds 85 %.
3. **Layout record in bank 2.** Two 128 KB sectors at `0x08100000` and `0x08120000`, written ping-pong so a power loss mid-write keeps the previous record. Fields: magic, schema version, sequence number, layout name, eight slots of (effect id, reserved settings bytes), menu settings (EQ, output level, 1/4" PEDAL / MIC), CRC32. On boot the newest valid record wins. An unknown effect id becomes `empty`. No valid record gives the default layout from the slots spec table.
4. **Apply.** Validate, write the record, fade the output over 50 ms, rebuild the chain in slot order, fade back in. Pots and the bypass toggle of a slot drive whatever card sits in it. `empty` passes audio straight through and ignores its controls.
5. **Chain order.** Slot order, left to right, always. No separate order list.
6. **Serial.** USB CDC, newline-delimited JSON, request and reply matched by `id`. Commands: `hello` (firmware version, registry with costs, live layout), `get_layout`, `set_layout`, `status` (live CPU, mute state), `enter_dfu`. Errors reply with a reason string the page shows.
7. **Version.** A compiled-in firmware version string, bumped by the release tag. Shown on the OLED and in `hello`.
8. **OLED menu.** Chain view (read only), knob readout on touch, EQ, output level, 1/4" PEDAL / MIC, MUTED, Update firmware (enters DFU, kept for when the app cannot talk). No slot editing.

### Recovery

Plain DFU. The ST ROM bootloader cannot be overwritten, so a bad image is recovered from the Update screen or the OLED menu, and from the BOOT0 button inside the box if the firmware does not boot. The image stays inside bank 1 so bank 2 and the layout survive every flash.

## Open questions resolved

1. Host tool. A Mac app installed and updated by one curl line, serving a local page (owner chose this over a hosted WebUSB page, which would have tied Adam to Chrome, and over in-person updates).
2. Where the layout lives. On the box, written from the page. Firmware carries all effects.
3. Bricking protection. Plain DFU with ROM bootloader and BOOT0 recovery. No A/B banks.
4. Library rule. One card per sound or variant, each card at most once, Empty allowed.
5. CPU budget. Hard stop at 85 %, enforced by the page and by the box.
6. Chain order. Slot order, left to right.
7. App runtime. Go single binary, no runtime to install. The emulator stays C++ and Dear ImGui as a developer tool.
8. Saved layouts. Kept on the Mac, as many as Adam likes.
9. Library contents. Each reverb engine and Harmony are their own cards; the reverb engine menu item is removed.
10. OLED editing. None. One editor.
11. Launch. Real `cubevox.app` in Applications, not a terminal command.
12. On-stage layout switching from the OLED. No. One live layout on the box.
13. Release channel. The repo is public, so GitHub Releases need no token.

## Decisions

1. App in Go, page in Web Awesome, firmware in the existing Arduino H7 tree.
2. Effect ids are strings and are never reused or renamed once shipped.
3. Layout record schema has a version and CRC from day one and carries a layout name so on-box switching can be added later without a format change.
4. 85 % CPU line uses summed peak costs. The box rejects, the page prevents.
5. 50 ms fade on apply.
6. dfu-util bundled in the app, no Homebrew dependency.
7. One GitHub release per tag carries firmware, app and install script together. The page compares both versions against the latest release.
8. Serial is JSON lines over USB CDC on 127.0.0.1 only; the app never listens on other interfaces.

## Recommended next steps

Build order, each step checkable before the next. The firmware foundation (directory tiers, host
tests, toolchain pin, fault handling) is in `2026-10-08-firmware-foundation-design.md` and sits
between step 1 part 1 (registry) and part 2 (runtime layout); its steps 1 to 4 are built.

0. Firmware clock (found 2026-10-08): the generic H743 variant runs PLL1 from HSI and USB from free-running HSI48 (±1 %, outside the USB FS ±0.25 % budget, no CRS in the core), and audio.cpp's PLL3M = 25 assumes HSE as the PLL source. Override `SystemClock_Config` in the sketch: HSE 25 MHz, PLL1 M 5 / N 192 / P 2 / Q 20 (480 MHz core, 48 MHz on PLL1Q), USB clock PLL1Q, HSI48 off. Also (Astra review, `docs/audits/2026-10-08-astra-usb-response.md`): hold USB soft-disconnect (DCTL.SDIS) until PA9 reads VBUS present and drop it when VBUS goes, since the box is self-powered; mark the configuration descriptor self-powered. Record the silicon revision (Y cannot run 480 MHz) and the ROM bootloader ID at 0x1FF1E7FE on the first board. Check: USB enumerates and SAI runs at 48 kHz on the board.
1. Firmware: effect registry with ids, knob descriptors and a costs table; bench mode to measure costs on the box.
2. Firmware: bank 2 layout record (ping-pong, CRC), boot-time load, default fallback, live apply with fade.
3. Firmware: compiled-in version string and the JSON serial command set; test against a Python script before the app exists.
4. App: Go server with `--fake`, serial relay, saved layouts, app bundle and `install.sh`.
5. Page: Layout screen first, then Layouts, then Update.
6. Release: GitHub Action on tag builds firmware with arduino-cli and the app, attaches all three files.
7. Docs: slots spec items 6 and 7, CLAUDE.md panel line, hardware spec item 17 (listed in the interview).
8. Later: a printable label strip from the page once lid legends are decided.
