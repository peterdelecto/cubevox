package box

import (
	"fmt"
	"io"
	"os"

	"golang.org/x/sys/unix"
)

// openSerial opens a CDC port raw, without becoming its controlling terminal. os.OpenFile keeps
// the descriptor non-blocking and on the runtime poller, so Close unblocks a pending read.
func openSerial(path string) (io.ReadWriteCloser, error) {
	f, err := os.OpenFile(path, os.O_RDWR|unix.O_NOCTTY, 0)
	if err != nil {
		return nil, fmt.Errorf("open %s: %w", path, err)
	}
	if err := setRaw(f); err != nil {
		f.Close()
		return nil, fmt.Errorf("raw mode %s: %w", path, err)
	}
	return f, nil
}

// setRaw applies the cfmakeraw flags with TCSANOW. Waiting for output to drain (TCSAFLUSH) would
// cut the boot report mid-line.
func setRaw(f *os.File) error {
	sc, err := f.SyscallConn()
	if err != nil {
		return err
	}
	var opErr error
	ctlErr := sc.Control(func(fd uintptr) {
		t, err := unix.IoctlGetTermios(int(fd), unix.TIOCGETA)
		if err != nil {
			opErr = err
			return
		}
		t.Iflag &^= unix.IGNBRK | unix.BRKINT | unix.PARMRK | unix.ISTRIP | unix.INLCR | unix.IGNCR | unix.ICRNL | unix.IXON
		t.Oflag &^= unix.OPOST
		t.Lflag &^= unix.ECHO | unix.ECHONL | unix.ICANON | unix.ISIG | unix.IEXTEN
		t.Cflag &^= unix.CSIZE | unix.PARENB
		t.Cflag |= unix.CS8 | unix.CLOCAL | unix.CREAD
		t.Cc[unix.VMIN] = 1 // VMIN 0 would read as EOF on the poller
		t.Cc[unix.VTIME] = 0
		opErr = unix.IoctlSetTermios(int(fd), unix.TIOCSETA, t)
	})
	if ctlErr != nil {
		return ctlErr
	}
	return opErr
}
