# cubevox app

Go helper that serves the layout page on 127.0.0.1 and relays it to the box over USB. Spec: `docs/specs/2026-10-15-mac-app-design.md`.

Run with no hardware: `go run . --fake --no-open` (flags: `--fake`, `--port N` with 0 for a free port, `--no-open`).
Test: `go vet ./... && go test ./...`. Page: `node tools/page_check.mjs` drives the page in headless Chrome through every spec behaviour (drag, swap, reverb replace, push error, keyboard, over the limit, bar states) and writes screenshots to `build/page-check/`. Build: `go build -ldflags "-X main.version=v0.01.00" -o cubevox-app .`
The app writes `~/Library/Application Support/cubevox/port`, exits 60 s after the last heartbeat, and a second launch only opens the browser.

API: `GET /api/state` (connected, fw, app, cards, layout, limits, update), `POST /api/layout` with `{"slots":[8 ids]}`
(the app fills the record name with the push time and returns the read-back or the box's error), `POST /api/update` (starts the update; a second call while one runs, or after a failure, is the page's Retry), `POST /api/heartbeat`.
Requests with a non-loopback Host or Origin are refused.

Update: the first heartbeat fetches the latest GitHub release once. `state.update` carries `version`, `available`, `step` and `error`.
The update replaces the app only and never writes the box. Firmware updates are the `.uf2` dragged onto the `CUBEVOX` drive (`docs/specs/2026-10-24-field-update-design.md`).
Steps run in order `download`, `app`, `restart`, `done`, and run only when the app is behind. A failure leaves `step` at the failed step with the error text, and Retry reuses a finished download.
On `restart` the app relaunches itself on the same port with `--no-open`, so the open page reloads onto the new copy.

Package: `app/bundle.sh` builds `dist/cubevox.app` and `dist/cubevox.app.tar.gz`, `app/install.sh` is the curl installer. `.github/workflows/release.yml` runs them on a `v*` tag.
