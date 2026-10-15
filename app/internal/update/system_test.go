package update

import (
	"os"
	"os/exec"
	"path/filepath"
	"testing"
)

func TestHasDFUDevice(t *testing.T) {
	const bootloader = `
  +-o STM32  BOOTLOADER@01100000  <class AppleUSBDevice, id 0x1>
    | {
    |   "idProduct" = 57105
    |   "idVendor" = 1155
    | }
`
	const other = `
  +-o Keyboard@01200000  <class AppleUSBDevice, id 0x2>
    | {
    |   "idProduct" = 57105
    |   "idVendor" = 1452
    | }
  +-o Hub@01300000  <class AppleUSBDevice, id 0x3>
    | {
    |   "idProduct" = 5710
    |   "idVendor" = 11550
    | }
`
	if !hasDFUDevice(bootloader) {
		t.Error("missed the bootloader")
	}
	if hasDFUDevice(other) {
		t.Error("matched ids from different devices or prefixes")
	}
	if hasDFUDevice(other+bootloader) != true {
		t.Error("missed the bootloader among others")
	}
}

func TestBundleOf(t *testing.T) {
	got, err := bundleOf("/Applications/cubevox.app/Contents/MacOS/cubevox")
	if err != nil || got != "/Applications/cubevox.app" {
		t.Fatalf("%q %v", got, err)
	}
	if _, err := bundleOf("/tmp/build/cubevox-app"); err == nil {
		t.Fatal("a bare binary is not in a bundle")
	}
}

func writeBundle(t *testing.T, root, marker string) string {
	t.Helper()
	exe := filepath.Join(root, "cubevox.app", "Contents", "MacOS", "cubevox")
	if err := os.MkdirAll(filepath.Dir(exe), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(exe, []byte(marker), 0o755); err != nil {
		t.Fatal(err)
	}
	return filepath.Join(root, "cubevox.app")
}

func TestSwapBundle(t *testing.T) {
	if _, err := exec.LookPath("tar"); err != nil {
		t.Skip("no tar")
	}
	src := t.TempDir()
	writeBundle(t, src, "new")
	tarball := filepath.Join(t.TempDir(), AppAsset)
	if out, err := exec.Command("tar", "-czf", tarball, "-C", src, "cubevox.app").CombinedOutput(); err != nil {
		t.Fatalf("tar: %v %s", err, out)
	}
	apps := t.TempDir()
	bundle := writeBundle(t, apps, "old")

	if err := SwapBundle(bundle, tarball); err != nil {
		t.Fatal(err)
	}
	got, err := os.ReadFile(filepath.Join(bundle, "Contents", "MacOS", "cubevox"))
	if err != nil || string(got) != "new" {
		t.Fatalf("bundle holds %q (%v)", got, err)
	}
	left, _ := os.ReadDir(apps)
	if len(left) != 1 {
		t.Errorf("leftovers beside the bundle: %v", left)
	}
}

func TestSwapBundleBadTarballKeepsOld(t *testing.T) {
	if _, err := exec.LookPath("tar"); err != nil {
		t.Skip("no tar")
	}
	apps := t.TempDir()
	bundle := writeBundle(t, apps, "old")
	bad := filepath.Join(t.TempDir(), AppAsset)
	if err := os.WriteFile(bad, []byte("not a tarball"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := SwapBundle(bundle, bad); err == nil {
		t.Fatal("want error")
	}
	got, err := os.ReadFile(filepath.Join(bundle, "Contents", "MacOS", "cubevox"))
	if err != nil || string(got) != "old" {
		t.Fatalf("old bundle lost: %q %v", got, err)
	}
}
