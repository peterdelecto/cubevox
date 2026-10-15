# cubevox app

Go helper that serves the layout page on 127.0.0.1 and relays it to the box over USB. Spec: `docs/specs/2026-10-15-mac-app-design.md`.

Run with no hardware: `go run . --fake --no-open` (flags: `--fake`, `--port N` with 0 for a free port, `--no-open`).
Test: `go vet ./... && go test ./...`. Build: `go build -ldflags "-X main.version=v0.01.00" -o cubevox-app .`
The app writes `~/Library/Application Support/cubevox/port`, exits 60 s after the last heartbeat, and a second launch only opens the browser.

API: `GET /api/state` (connected, fw, app, cards, layout, limits, update), `POST /api/layout` with `{"slots":[8 ids]}`
(the app fills the record name with the push time and returns the read-back or the box's error), `POST /api/update` (starts the update; a second call while one runs, or after a failure, is the page's Retry), `POST /api/heartbeat`.
Requests with a non-loopback Host or Origin are refused.

Update: the first heartbeat fetches the latest GitHub release once. `state.update` carries `version`, `available`, `step` and `error`.
Steps run in order `backup`, `download`, `dfu`, `app`, `restart`, `done`; the firmware steps run only when the box is behind and the app steps only when the app is behind.
A failure leaves `step` at the failed step with the dfu-util output in `error`; the box stays in DFU and Retry resumes there.
On `restart` the app relaunches itself on the same port with `--no-open`, so the open page reloads onto the new copy.

Package: `app/dfu-util-static.sh [out]` builds a static universal dfu-util, `DFU_UTIL=<that> app/bundle.sh` builds `dist/cubevox.app` and `dist/cubevox.app.tar.gz`, `app/install.sh` is the curl installer. `.github/workflows/release.yml` runs all three on a `v*` tag.
