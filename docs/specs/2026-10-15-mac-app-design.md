# Mac app design (owner, 2026-10-15)

Supersedes the Page, Install and Update parts of `2026-10-08-update-and-slots-design.md`. The
firmware side of that spec (registry, record, protocol, apply) stands and is built; the serial
command set is `2026-10-12-serial-protocol-design.md`.

## Plan

Adam installs `cubevox.app` with one curl line and double-clicks it. His browser opens one page.
Eight slots sit on top in face order, a bank of effect cards below. He drags cards into slots,
a meter shows how much processing room the layout takes, and Push sends it to the box over
USB-C. The box applies it live and keeps it. There is nothing to save on the Mac. The bottom
bar shows the connection and, only when a release is newer, one Update button that updates the
firmware and the app in one click.

## Pieces

| Piece | Where | What |
|---|---|---|
| App | `app/`, Go, one binary inside `cubevox.app` | Serves the page on 127.0.0.1, relays JSON to the box over USB CDC, runs dfu-util, checks GitHub Releases, updates itself. |
| Page | `app/web/`, embedded in the binary | One screen. Web Awesome Default theme, dark, vendored. |
| Fake box | `app/internal/fake` | Answers the protocol from a fixture so the page runs with no hardware. |
| Fixture | `app/fixtures/hello.json` | The firmware's real `hello` reply for the default layout, written by the host tool `hello_dump` from the core sources. |
| Release | GitHub Releases on `peterdelecto/cubevox` | One release per tag `vX.YY.ZZ` with `cubevox.bin`, `cubevox.app.tar.gz`, `install.sh`, `dfu-util`. |

## App

1. **Install.** `curl -fsSL https://github.com/peterdelecto/cubevox/releases/latest/download/install.sh | sh`
   downloads `cubevox.app.tar.gz`, unpacks it into `/Applications` (falls back to
   `~/Applications`), replaces an older copy. curl sets no quarantine flag.
2. **Launch.** Double-click. The app is a hidden helper (`LSUIElement` true, no dock icon, no
   window). It picks a free port on 127.0.0.1, writes `~/Library/Application Support/cubevox/port`,
   opens the default browser on it. A second launch finds a live port file and only opens the
   browser. The page sends a heartbeat every 10 s; 60 s after the last heartbeat the app exits and
   removes the port file.
3. **Serial.** The app opens the first `/dev/cu.usbmodem*` port that answers `hello` within 2 s,
   only while a page is open, and closes it on idle exit. It polls `status` once a second for the
   bottom bar. Only lines starting with `{` are read. Disconnect is a read error or a vanished
   device; the app retries every 2 s.
4. **Layout.** `POST /api/layout` with eight card ids. The app fills the record name the box
   requires with the push date (`2026-10-15 14:02`), sends `set_layout`, then `get_layout`, and
   returns the read-back. The box's error string comes back unchanged for the page to show.
5. **Cards.** With a box, the card list is the box's `hello`. Without one, it is the embedded
   fixture, which matches the firmware of the same release.
6. **Update.** On page open the app fetches
   `https://api.github.com/repos/peterdelecto/cubevox/releases/latest` once (no token, public
   repo) and compares the tag to its own version and the box's `fw`. If either is behind, the bar
   shows `Update to vX.YY.ZZ`. One click runs, in order: back the live layout up to
   `~/Library/Application Support/cubevox/last-layout.json`; if the firmware is behind, send
   `enter_dfu`, wait for `0483:df11`, run the bundled dfu-util on the downloaded `cubevox.bin`,
   wait for the CDC port to come back, read `hello`; if the app is behind, download the app
   tarball, unpack beside the bundle, swap directories, relaunch, and the page reloads. The page
   shows each step and the dfu-util output on failure with a Retry button; the box stays in DFU
   until a flash succeeds.
7. **Versions.** One tag per release. `VERSION.txt` holds the tag; `git describe` gives the
   firmware its string; the Go binary gets the same tag by `-ldflags -X`.
8. **Security.** Listens on 127.0.0.1 only. No request leaves the Mac except the GitHub release
   fetch and the asset downloads.

## HTTP API (page to app)

| Route | Body | Reply |
|---|---|---|
| `GET /api/state` | | `connected`, `fw`, `app`, `cards` (from `hello`: id, name, group, cost, knobs), `layout` (`slots`, eight ids), `limits` (`mean` 75, `worst` 95, `fixed` cost), `update` (`version`, `available`, `step`, `error`) |
| `POST /api/layout` | `{"slots":[8 ids]}` | `{"ok":true,"layout":{...}}` or `{"ok":false,"error":"..."}` |
| `POST /api/update` | | `{"ok":true}`; progress through `state.update.step` |
| `POST /api/heartbeat` | | `{"ok":true}` |

The page polls `GET /api/state` once a second.

## Page

1. **Slots.** Eight drop targets in one row, numbered, left to right as on the face. A placed
   card shows its name and its two knob names in pot order. An empty slot says "empty".
2. **Bank.** Every card except `empty`, name and knob names, no cost. Drag into a slot to place,
   drag a slot's card back to the bank to empty the slot, drop on a filled slot to swap. A placed
   card greys in the bank. Dropping a reverb while another reverb is placed empties that other
   slot, so the layout keeps one reverb without a clearing step (owner 2026-10-15).
3. **Meter.** "Processing room", one bar. The budget is shown as 100 %: the bar reads
   `max(meanSum / 75, worstSum / 95)` with the fixed term in both sums, as "76 % used, 24 % left".
   Over 100 % the bar turns red and Push is disabled. The box checks the same two lines and
   rejects; the page prevents.
4. **Push.** Sends the layout. The slots then show what the box read back. A box error shows
   under the bar in the box's words.
5. **Bottom bar.** `Box connected` or `Connect the box`, firmware version, and the Update
   button only when a release is newer. During an update the bar shows the step.
6. **No box.** Slots and bank work from the embedded card list; Push is disabled.
7. **Look.** Web Awesome Default theme, `wa-dark`, vendored under `app/web/vendor/`. Tokens only,
   through `ui-foundation` before the first markup.

## Release

1. CI job on tag `v*`: build `cubevox.bin` with `firmware/build.sh`, build the Go binary as a
   universal binary (`GOARCH=arm64` and `amd64`, `lipo`), bundle `cubevox.app` with
   `Info.plist`, dfu-util and the binary, tar it, attach `cubevox.bin`, `cubevox.app.tar.gz`,
   `install.sh` to the release.
2. dfu-util is built static against libusb on the runner once per release, universal.

## Open questions resolved

1. App runtime: Go single binary (owner 2026-10-15, round 1).
2. Page scope: layout only; EQ, output level and pedal/mic stay on the OLED.
3. Versions: one tag stream for emulator, firmware and app.
4. App presence: hidden helper, the browser tab is the app, idle exit after 60 s.
5. Port ownership: only while a page is open, status once a second.
6. Fake box source: the firmware's `hello` written by a host tool, never retyped.
7. Push model: a button, with read-back. Not push-on-drop.
8. Offline: Web Awesome vendored, no CDN.
9. Saved layouts: none. "Not complicated enough to need to save layouts" (owner).
10. Screens: one.
11. Update: one button, shown only when behind, one click does firmware then app.
12. dfu-util: bundled, static. Flash failure: Retry, box stays in DFU.
13. No box: page still works from the embedded card list, Push greyed.
14. Card cost: not shown per card; the meter is the only processing number.
15. Meter scale: the 75 % budget is Adam's 100 %.
16. Theme: Web Awesome Default, dark.

## Decisions

1. One screen, one layout, no files for Adam.
2. The page prevents what the box rejects (duplicates, two reverbs, over budget), using the same
   costs the box reports in `hello`.
3. The record name is app-filled with the push date.
4. The card list has one source, the firmware registry, reaching the page through `hello` or the
   fixture `hello_dump` writes.
5. The app never listens off 127.0.0.1 and never calls out except to GitHub Releases.

## Verification

1. `go test ./...` in `app/`: fake box answers every command; layout validation matches the
   firmware rules on the fixture; the API routes; update version comparison; idle exit timing.
2. `hello_dump` runs under ctest and the fixture matches its output (test fails when the registry
   changes without a fixture refresh).
3. By hand: `cubevox --fake`, the page in Safari and Chrome, drag and drop, meter at the limit,
   push error text, bottom bar states.
4. With the box: `tools/protocol_check.py` still passes with the app closed; the page pushes a
   layout and the OLED chain view follows.

## Build order

1. `hello_dump` host tool and `app/fixtures/hello.json`.
2. Go module: serial link, fake, layout rules, API, lifetime, `--fake`.
3. Page.
4. Update flow, bundle script, `install.sh`, release job.
