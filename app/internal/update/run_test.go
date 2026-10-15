package update

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"cubevox.app/internal/box"
)

// fakeBox models a box that is connected, in DFU, or running new firmware after a flash.
type fakeBox struct {
	fw        string
	connected bool
	inDFU     bool
	dfuCalls  int
}

func (b *fakeBox) Hello() (box.Hello, error) {
	if !b.connected {
		return box.Hello{}, box.ErrNotConnected
	}
	return box.Hello{FW: b.fw}, nil
}

func (b *fakeBox) GetLayout() (box.Layout, error) {
	return box.Layout{Name: "2026-10-15 14:02", Slots: []string{"a", "b"}}, nil
}
func (b *fakeBox) SetLayout(string, []string) error { return nil }
func (b *fakeBox) Status() (box.Status, error)      { return nil, nil }
func (b *fakeBox) EnterDFU() error {
	b.dfuCalls++
	b.connected, b.inDFU = false, true
	return nil
}

// rig is a Deps whose collaborators are all fakes that record what they were asked to do.
type rig struct {
	deps       Deps
	box        *fakeBox
	steps      []string
	gets       []string
	flashes    int
	flashErr   error // returned by Flash while set
	swapped    string
	relaunched bool
}

func newRig(t *testing.T, app, fw string) *rig {
	t.Helper()
	dir := t.TempDir()
	r := &rig{box: &fakeBox{fw: fw, connected: true}}
	r.deps = Deps{
		Info: Info{Tag: "v0.02.00", BinURL: "u/bin", BinSize: 4, AppURL: "u/app", AppSize: 3},
		App:  app, Box: r.box,
		Step:       func(n string) { r.steps = append(r.steps, n) },
		BackupPath: filepath.Join(dir, "support", "last-layout.json"),
		WorkDir:    filepath.Join(dir, "work"),
		Get: func(_ context.Context, url, dest string) error {
			r.gets = append(r.gets, url)
			n := 4
			if url == "u/app" {
				n = 3
			}
			return os.WriteFile(dest, make([]byte, n), 0o644)
		},
		InDFU:   func(context.Context) bool { return r.box.inDFU },
		WaitDFU: func(context.Context) error { return nil },
		Flash: func(context.Context, string) (string, error) {
			r.flashes++
			if r.flashErr != nil {
				return "dfu-util: No DFU capable USB device available", r.flashErr
			}
			r.box.inDFU, r.box.connected, r.box.fw = false, true, "v0.02.00"
			return "Download done.", nil
		},
		Swap:     func(_ context.Context, tarball string) error { r.swapped = tarball; return nil },
		Relaunch: func() error { r.relaunched = true; return nil },
		Poll:     time.Millisecond, BoxWait: 50 * time.Millisecond,
	}
	return r
}

func (r *rig) run(t *testing.T) error {
	t.Helper()
	return Run(context.Background(), r.deps)
}

func (r *rig) wantSteps(t *testing.T, want string) {
	t.Helper()
	if got := strings.Join(r.steps, ","); got != want {
		t.Errorf("steps %s, want %s", got, want)
	}
}

func TestRunFirmwareOnly(t *testing.T) {
	r := newRig(t, "v0.02.00", "v0.01.00")
	if err := r.run(t); err != nil {
		t.Fatal(err)
	}
	r.wantSteps(t, "backup,download,dfu,done")
	if len(r.gets) != 1 || r.gets[0] != "u/bin" || r.flashes != 1 || r.swapped != "" || r.relaunched {
		t.Errorf("gets %v flashes %d swapped %q relaunched %v", r.gets, r.flashes, r.swapped, r.relaunched)
	}
	data, err := os.ReadFile(r.deps.BackupPath)
	if err != nil || !strings.Contains(string(data), `"slots"`) {
		t.Errorf("backup %q err %v", data, err)
	}
}

func TestRunAppOnly(t *testing.T) {
	r := newRig(t, "v0.01.00", "v0.02.00")
	if err := r.run(t); err != nil {
		t.Fatal(err)
	}
	r.wantSteps(t, "backup,download,app,restart")
	if len(r.gets) != 1 || r.gets[0] != "u/app" || r.box.dfuCalls != 0 || r.flashes != 0 {
		t.Errorf("gets %v dfu %d flashes %d", r.gets, r.box.dfuCalls, r.flashes)
	}
	if filepath.Base(r.swapped) != AppAsset || !r.relaunched {
		t.Errorf("swapped %q relaunched %v", r.swapped, r.relaunched)
	}
}

func TestRunBothFlashesBeforeSwapping(t *testing.T) {
	r := newRig(t, "v0.01.00", "v0.01.00")
	if err := r.run(t); err != nil {
		t.Fatal(err)
	}
	r.wantSteps(t, "backup,download,dfu,app,restart")
	if len(r.gets) != 2 || r.flashes != 1 || r.swapped == "" || !r.relaunched {
		t.Errorf("gets %v flashes %d swapped %q", r.gets, r.flashes, r.swapped)
	}
}

func TestRunAppOnlyWithoutBox(t *testing.T) {
	r := newRig(t, "v0.01.00", "")
	r.box.connected = false
	if err := r.run(t); err != nil {
		t.Fatal(err)
	}
	r.wantSteps(t, "download,app,restart")
	if _, err := os.Stat(r.deps.BackupPath); err == nil {
		t.Error("backup written with no box")
	}
}

func TestRunNothingToDo(t *testing.T) {
	r := newRig(t, "v0.02.00", "v0.02.00")
	if err := r.run(t); err != nil {
		t.Fatal(err)
	}
	r.wantSteps(t, "done")
	if len(r.gets) != 0 {
		t.Errorf("downloaded %v", r.gets)
	}
}

func TestRunFlashFailureThenRetry(t *testing.T) {
	r := newRig(t, "v0.02.00", "v0.01.00")
	r.flashErr = errors.New("exit status 74")
	err := r.run(t)
	if err == nil || !strings.Contains(err.Error(), "No DFU capable USB device") {
		t.Fatalf("error %v lacks dfu-util output", err)
	}
	if !r.box.inDFU || r.box.dfuCalls != 1 {
		t.Fatalf("box left inDFU=%v after %d enter_dfu", r.box.inDFU, r.box.dfuCalls)
	}

	r.flashErr, r.steps = nil, nil
	if err := r.run(t); err != nil {
		t.Fatalf("retry: %v", err)
	}
	r.wantSteps(t, "download,dfu,done")
	if r.box.dfuCalls != 1 {
		t.Errorf("retry sent enter_dfu again (%d)", r.box.dfuCalls)
	}
	if len(r.gets) != 1 {
		t.Errorf("retry downloaded again: %v", r.gets)
	}
	if r.flashes != 2 {
		t.Errorf("flashes %d, want 2", r.flashes)
	}
}

func TestRunBoxNeverComesBack(t *testing.T) {
	r := newRig(t, "v0.02.00", "v0.01.00")
	r.deps.Flash = func(context.Context, string) (string, error) { return "ok", nil }
	err := r.run(t)
	if err == nil || !strings.Contains(err.Error(), "did not come back") {
		t.Fatalf("error %v", err)
	}
}

func TestRunBoxComesBackWithOldFirmware(t *testing.T) {
	r := newRig(t, "v0.02.00", "v0.01.00")
	r.deps.Flash = func(context.Context, string) (string, error) {
		r.box.connected, r.box.inDFU = true, false
		return "ok", nil
	}
	if err := r.run(t); err == nil || !strings.Contains(err.Error(), "v0.01.00") {
		t.Fatalf("error %v", err)
	}
}

func TestRunWrongDownloadSize(t *testing.T) {
	r := newRig(t, "v0.02.00", "v0.01.00")
	r.deps.Info.BinSize = 9
	err := r.run(t)
	if err == nil || !strings.Contains(err.Error(), "size") {
		t.Fatalf("error %v", err)
	}
	if _, statErr := os.Stat(filepath.Join(r.deps.WorkDir, FirmwareAsset)); statErr == nil {
		t.Error("bad download kept")
	}
	if r.box.dfuCalls != 0 {
		t.Error("entered DFU with a bad image")
	}
}

func TestRunMissingAsset(t *testing.T) {
	r := newRig(t, "v0.02.00", "v0.01.00")
	r.deps.Info.BinURL = ""
	if err := r.run(t); err == nil || !strings.Contains(err.Error(), FirmwareAsset) {
		t.Fatalf("error %v", err)
	}
}
