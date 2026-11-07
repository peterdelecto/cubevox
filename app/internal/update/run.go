package update

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
)

// Step names the page shows, in run order.
const (
	StepDownload = "download"
	StepApp      = "app"
	StepRestart  = "restart" // the new bundle is in place; the page reloads when the app is back
	StepDone     = "done"
)

// Deps is everything Run touches outside its own logic, so tests can replace each piece.
type Deps struct {
	Info Info
	App  string // this app's version
	Step func(name string)

	WorkDir string // the download lands here and survives between attempts

	Get      func(ctx context.Context, url, dest string) error
	Swap     func(ctx context.Context, tarball string) error // replace the running bundle
	Relaunch func() error                                    // start the new bundle and end this process
}

// Run replaces this app with the release's bundle when the release is newer. It never touches
// the box, because firmware updates are the .uf2 dragged onto the CUBEVOX drive. A failure
// returns an error, and the page's Retry is a second call that reuses a finished download.
func Run(ctx context.Context, d Deps) error {
	if !Newer(d.Info.Tag, d.App) {
		d.Step(StepDone)
		return nil
	}
	d.Step(StepDownload)
	if err := os.MkdirAll(d.WorkDir, 0o755); err != nil {
		return fmt.Errorf("download: %w", err)
	}
	tarball := filepath.Join(d.WorkDir, AppAsset)
	if err := d.fetch(ctx, d.Info.AppURL, tarball, d.Info.AppSize); err != nil {
		return err
	}
	return d.replaceApp(ctx, tarball)
}

// fetch downloads url to dest unless dest already has the release size. A wrong size removes the
// file so the next attempt starts clean.
func (d Deps) fetch(ctx context.Context, url, dest string, size int64) error {
	name := filepath.Base(dest)
	if url == "" {
		return fmt.Errorf("download: release %s has no %s", d.Info.Tag, name)
	}
	if sizeIs(dest, size) {
		return nil
	}
	if err := d.Get(ctx, url, dest); err != nil {
		return fmt.Errorf("download %s: %w", name, err)
	}
	if !sizeIs(dest, size) {
		os.Remove(dest)
		return fmt.Errorf("download %s: size differs from the release (%d bytes expected)", name, size)
	}
	return nil
}

// sizeIs reports whether path exists and, when want is known (nonzero), has that size.
func sizeIs(path string, want int64) bool {
	fi, err := os.Stat(path)
	if err != nil || fi.Size() == 0 {
		return false
	}
	return want == 0 || fi.Size() == want
}

func (d Deps) replaceApp(ctx context.Context, tarball string) error {
	d.Step(StepApp)
	if err := d.Swap(ctx, tarball); err != nil {
		return fmt.Errorf("replace app: %w", err)
	}
	d.Step(StepRestart)
	if err := d.Relaunch(); err != nil {
		return fmt.Errorf("relaunch: %w", err)
	}
	return nil
}
