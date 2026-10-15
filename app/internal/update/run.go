package update

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"time"

	"cubevox.app/internal/box"
)

// Step names the page shows, in run order.
const (
	StepBackup   = "backup"
	StepDownload = "download"
	StepDFU      = "dfu"
	StepApp      = "app"
	StepRestart  = "restart" // the new bundle is in place; the page reloads when the app is back
	StepDone     = "done"
)

// Defaults for Deps.Poll and Deps.BoxWait.
const (
	defaultPoll    = 500 * time.Millisecond
	defaultBoxWait = 30 * time.Second
)

// Deps is everything Run touches outside its own logic, so tests can replace each piece.
type Deps struct {
	Info Info
	App  string // this app's version
	Box  box.Box
	Step func(name string)

	BackupPath string // where the live layout is saved before anything changes
	WorkDir    string // downloads land here and survive between attempts

	Get      func(ctx context.Context, url, dest string) error
	InDFU    func(ctx context.Context) bool                        // a 0483:df11 device is attached now
	WaitDFU  func(ctx context.Context) error                       // blocks until one appears
	Flash    func(ctx context.Context, bin string) (string, error) // dfu-util; returns its output
	Swap     func(ctx context.Context, tarball string) error       // replace the running bundle
	Relaunch func() error                                          // start the new bundle and end this process

	Poll    time.Duration // box poll interval after a flash
	BoxWait time.Duration // how long the box has to come back
}

type plan struct {
	firmware  bool // flash the box
	app       bool // replace this app
	connected bool // a box answered hello, so its layout can be backed up
	inDFU     bool // the box is already in DFU from an earlier failed attempt
}

// Run performs the update. It starts from the first incomplete step on every call, so the
// page's Retry is a second call: finished downloads are reused and a box already in DFU skips
// the enter_dfu command. A failure returns an error that carries dfu-util's output and leaves
// the box where it is.
func Run(ctx context.Context, d Deps) error {
	p, err := d.plan(ctx)
	if err != nil {
		return err
	}
	if !p.firmware && !p.app {
		d.Step(StepDone)
		return nil
	}
	if p.connected {
		if err := d.backup(); err != nil {
			return err
		}
	}
	bin, tarball, err := d.download(ctx, p)
	if err != nil {
		return err
	}
	if p.firmware {
		if err := d.flashBox(ctx, p, bin); err != nil {
			return err
		}
	}
	if p.app {
		return d.replaceApp(ctx, tarball)
	}
	d.Step(StepDone)
	return nil
}

// plan decides which parts are due. A box that does not answer is either absent (app-only
// update) or sitting in DFU after a failed flash (firmware due, no enter_dfu needed).
func (d Deps) plan(ctx context.Context) (plan, error) {
	p := plan{app: Newer(d.Info.Tag, d.App)}
	hello, err := d.Box.Hello()
	switch {
	case err == nil:
		p.connected = true
		p.firmware = Newer(d.Info.Tag, hello.FW)
	case errors.Is(err, box.ErrNotConnected):
		if d.InDFU(ctx) {
			p.inDFU, p.firmware = true, true
		}
	default:
		return p, fmt.Errorf("read box version: %w", err)
	}
	return p, nil
}

func (d Deps) backup() error {
	d.Step(StepBackup)
	layout, err := d.Box.GetLayout()
	if err != nil {
		return fmt.Errorf("back up layout: %w", err)
	}
	data, err := json.MarshalIndent(layout, "", "  ")
	if err != nil {
		return fmt.Errorf("back up layout: %w", err)
	}
	if err := os.MkdirAll(filepath.Dir(d.BackupPath), 0o755); err != nil {
		return fmt.Errorf("back up layout: %w", err)
	}
	if err := os.WriteFile(d.BackupPath, append(data, '\n'), 0o644); err != nil {
		return fmt.Errorf("back up layout: %w", err)
	}
	return nil
}

// download fetches the assets the plan needs and returns their paths ("" for one not needed).
func (d Deps) download(ctx context.Context, p plan) (bin, tarball string, err error) {
	d.Step(StepDownload)
	if err := os.MkdirAll(d.WorkDir, 0o755); err != nil {
		return "", "", fmt.Errorf("download: %w", err)
	}
	if p.firmware {
		bin = filepath.Join(d.WorkDir, FirmwareAsset)
		if err := d.fetch(ctx, d.Info.BinURL, bin, d.Info.BinSize); err != nil {
			return "", "", err
		}
	}
	if p.app {
		tarball = filepath.Join(d.WorkDir, AppAsset)
		if err := d.fetch(ctx, d.Info.AppURL, tarball, d.Info.AppSize); err != nil {
			return "", "", err
		}
	}
	return bin, tarball, nil
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

func (d Deps) flashBox(ctx context.Context, p plan, bin string) error {
	d.Step(StepDFU)
	if !p.inDFU {
		if err := d.Box.EnterDFU(); err != nil {
			return fmt.Errorf("enter DFU: %w", err)
		}
		if err := d.WaitDFU(ctx); err != nil {
			return fmt.Errorf("wait for DFU: %w", err)
		}
	}
	out, err := d.Flash(ctx, bin)
	if err != nil {
		return fmt.Errorf("dfu-util failed: %w\n%s", err, out)
	}
	return d.waitForBox(ctx)
}

// waitForBox polls hello until the box reports the release tag or BoxWait runs out.
func (d Deps) waitForBox(ctx context.Context) error {
	poll, limit := d.Poll, d.BoxWait
	if poll == 0 {
		poll = defaultPoll
	}
	if limit == 0 {
		limit = defaultBoxWait
	}
	deadline := time.Now().Add(limit)
	last := "no answer"
	for time.Now().Before(deadline) {
		h, err := d.Box.Hello()
		if err == nil {
			if Tag(h.FW) == Tag(d.Info.Tag) {
				return nil
			}
			last = "firmware " + h.FW
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-time.After(poll):
		}
	}
	return fmt.Errorf("box did not come back as %s after flashing (%s)", d.Info.Tag, last)
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
