//go:build !darwin

package box

import (
	"errors"
	"io"
)

// openSerial exists so the package builds off the Mac; the app only talks to a box on macOS.
func openSerial(path string) (io.ReadWriteCloser, error) {
	return nil, errors.New("serial link needs macOS")
}
