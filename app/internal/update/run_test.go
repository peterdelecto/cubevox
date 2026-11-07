package update

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// rig is a Deps whose collaborators are all fakes that record what they were asked to do.
type rig struct {
	deps       Deps
	steps      []string
	gets       []string
	swapped    string
	swapErr    error
	relaunched bool
}

func newRig(t *testing.T, app string) *rig {
	t.Helper()
	r := &rig{}
	r.deps = Deps{
		Info:    Info{Tag: "v0.02.00", AppURL: "u/app", AppSize: 3},
		App:     app,
		Step:    func(n string) { r.steps = append(r.steps, n) },
		WorkDir: filepath.Join(t.TempDir(), "work"),
		Get: func(_ context.Context, url, dest string) error {
			r.gets = append(r.gets, url)
			return os.WriteFile(dest, make([]byte, 3), 0o644)
		},
		Swap: func(_ context.Context, tarball string) error {
			r.swapped = tarball
			return r.swapErr
		},
		Relaunch: func() error { r.relaunched = true; return nil },
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

func TestRunReplacesOlderApp(t *testing.T) {
	r := newRig(t, "v0.01.00")
	if err := r.run(t); err != nil {
		t.Fatal(err)
	}
	r.wantSteps(t, "download,app,restart")
	if len(r.gets) != 1 || r.gets[0] != "u/app" {
		t.Errorf("gets %v", r.gets)
	}
	if filepath.Base(r.swapped) != AppAsset || !r.relaunched {
		t.Errorf("swapped %q relaunched %v", r.swapped, r.relaunched)
	}
}

func TestRunNothingToDo(t *testing.T) {
	r := newRig(t, "v0.02.00")
	if err := r.run(t); err != nil {
		t.Fatal(err)
	}
	r.wantSteps(t, "done")
	if len(r.gets) != 0 || r.swapped != "" {
		t.Errorf("downloaded %v swapped %q", r.gets, r.swapped)
	}
}

func TestRunSwapFailureThenRetryReusesDownload(t *testing.T) {
	r := newRig(t, "v0.01.00")
	r.swapErr = errors.New("disk full")
	if err := r.run(t); err == nil || !strings.Contains(err.Error(), "disk full") {
		t.Fatalf("error %v", err)
	}
	if r.relaunched {
		t.Error("relaunched after a failed swap")
	}

	r.swapErr, r.steps = nil, nil
	if err := r.run(t); err != nil {
		t.Fatalf("retry: %v", err)
	}
	r.wantSteps(t, "download,app,restart")
	if len(r.gets) != 1 {
		t.Errorf("retry downloaded again: %v", r.gets)
	}
}

func TestRunWrongDownloadSize(t *testing.T) {
	r := newRig(t, "v0.01.00")
	r.deps.Info.AppSize = 9
	err := r.run(t)
	if err == nil || !strings.Contains(err.Error(), "size") {
		t.Fatalf("error %v", err)
	}
	if _, statErr := os.Stat(filepath.Join(r.deps.WorkDir, AppAsset)); statErr == nil {
		t.Error("bad download kept")
	}
	if r.swapped != "" {
		t.Error("swapped in a bad download")
	}
}

func TestRunMissingAsset(t *testing.T) {
	r := newRig(t, "v0.01.00")
	r.deps.Info.AppURL = ""
	if err := r.run(t); err == nil || !strings.Contains(err.Error(), AppAsset) {
		t.Fatalf("error %v", err)
	}
}
