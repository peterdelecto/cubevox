package update

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"syscall"
	"time"
)

const (
	// pageGrace gives the page time to poll once and see the restart step; handOff keeps the
	// new process from starting while this one still holds the port and the port file.
	pageGrace = 2 * time.Second
	handOff   = 3
)

// SystemDeps returns Deps wired to this Mac: the network and the running bundle. port is the
// port this process serves, reused by the relaunched app so the open page reloads onto it;
// quit ends this process.
func SystemDeps(info Info, app string, port int, quit func()) Deps {
	return Deps{
		Info: info, App: app,
		WorkDir: filepath.Join(os.TempDir(), "cubevox-update-"+filepath.Base(info.Tag)),
		Get:     download,
		Swap: func(_ context.Context, tarball string) error {
			bundle, err := BundlePath()
			if err != nil {
				return err
			}
			return SwapBundle(bundle, tarball)
		},
		Relaunch: func() error {
			bundle, err := BundlePath()
			if err != nil {
				return err
			}
			return relaunch(bundle, port, quit)
		},
	}
}

// BundlePath returns the running .app directory, or an error when the binary runs outside one.
func BundlePath() (string, error) {
	exe, err := os.Executable()
	if err != nil {
		return "", fmt.Errorf("find executable: %w", err)
	}
	return bundleOf(exe)
}

// bundleOf maps <bundle>.app/Contents/MacOS/<exe> to <bundle>.app.
func bundleOf(exe string) (string, error) {
	macos := filepath.Dir(exe)
	contents := filepath.Dir(macos)
	bundle := filepath.Dir(contents)
	if filepath.Base(macos) != "MacOS" || filepath.Base(contents) != "Contents" || filepath.Ext(bundle) != ".app" {
		return "", fmt.Errorf("%s is not inside an .app bundle", exe)
	}
	return bundle, nil
}

func download(ctx context.Context, url, dest string) error {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return err
	}
	req.Header.Set("User-Agent", "cubevox-app")
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return errors.New(resp.Status)
	}
	// Write beside the target and rename, so a cut connection never leaves a file that
	// looks complete.
	part := dest + ".part"
	f, err := os.Create(part)
	if err != nil {
		return err
	}
	_, copyErr := io.Copy(f, resp.Body)
	closeErr := f.Close()
	if err := errors.Join(copyErr, closeErr); err != nil {
		os.Remove(part)
		return err
	}
	return os.Rename(part, dest)
}

// SwapBundle replaces the bundle directory with the one in tarball (which holds cubevox.app at
// its top). The new copy is unpacked beside the bundle so every rename stays on one volume; the
// old copy is kept as <bundle>.old until the new one is in place.
func SwapBundle(bundle, tarball string) error {
	parent := filepath.Dir(bundle)
	stage, err := os.MkdirTemp(parent, ".cubevox-update-")
	if err != nil {
		return fmt.Errorf("stage beside bundle: %w", err)
	}
	defer os.RemoveAll(stage)
	if out, err := exec.Command("/usr/bin/tar", "-xzf", tarball, "-C", stage).CombinedOutput(); err != nil {
		return fmt.Errorf("unpack: %w: %s", err, bytes.TrimSpace(out))
	}
	fresh := filepath.Join(stage, "cubevox.app")
	if _, err := os.Stat(filepath.Join(fresh, "Contents", "MacOS", "cubevox")); err != nil {
		return fmt.Errorf("tarball has no cubevox.app: %w", err)
	}
	old := bundle + ".old"
	if err := os.RemoveAll(old); err != nil {
		return fmt.Errorf("clear %s: %w", old, err)
	}
	if err := os.Rename(bundle, old); err != nil {
		return fmt.Errorf("move old bundle aside: %w", err)
	}
	if err := os.Rename(fresh, bundle); err != nil {
		if undo := os.Rename(old, bundle); undo != nil {
			return fmt.Errorf("install new bundle: %w (restoring the old one also failed: %v)", err, undo)
		}
		return fmt.Errorf("install new bundle: %w", err)
	}
	if err := os.RemoveAll(old); err != nil {
		return fmt.Errorf("remove %s: %w", old, err)
	}
	return nil
}

// relaunch starts the bundle again on the same port and ends this process. The open page keeps
// polling that port, so it reloads onto the new app without a second tab (--no-open).
func relaunch(bundle string, port int, quit func()) error {
	time.Sleep(pageGrace)
	script := fmt.Sprintf(`sleep %d; exec /usr/bin/open -n -a "$0" --args --port "$1" --no-open`, handOff)
	cmd := exec.Command("/bin/sh", "-c", script, bundle, strconv.Itoa(port))
	cmd.SysProcAttr = &syscall.SysProcAttr{Setsid: true}
	if err := cmd.Start(); err != nil {
		return fmt.Errorf("start open: %w", err)
	}
	if err := cmd.Process.Release(); err != nil {
		return fmt.Errorf("release open: %w", err)
	}
	quit()
	return nil
}
