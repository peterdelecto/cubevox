// Package fake is a box that answers the protocol from the hello fixture, so the page and the
// API run with no hardware.
package fake

import (
	"errors"
	"sync"

	"cubevox.app/internal/box"
)

// Box implements box.Box in memory with the firmware's layout rules.
type Box struct {
	mu     sync.Mutex
	hello  box.Hello
	layout box.Layout
	dfu    bool
}

// New builds a fake from a hello reply, normally the fixture.
func New(h box.Hello) *Box {
	return &Box{hello: h, layout: copyLayout(h.Layout)}
}

func copyLayout(l box.Layout) box.Layout {
	return box.Layout{Name: l.Name, Slots: append([]string(nil), l.Slots...)}
}

// Hello implements box.Box. The hello carries the current layout, as the real box's does.
func (f *Box) Hello() (box.Hello, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	h := f.hello
	h.Layout = copyLayout(f.layout)
	return h, nil
}

// GetLayout implements box.Box.
func (f *Box) GetLayout() (box.Layout, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	return copyLayout(f.layout), nil
}

// SetLayout implements box.Box. A refusal is a *box.RemoteError with the firmware's wording.
func (f *Box) SetLayout(name string, slots []string) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	if err := Validate(f.hello.Cards, name, slots); err != nil {
		return &box.RemoteError{Text: err.Error()}
	}
	f.layout = box.Layout{Name: name, Slots: append([]string(nil), slots...)}
	return nil
}

// Status implements box.Box with an idle box.
func (f *Box) Status() (box.Status, error) {
	return box.Status{"cpu_mean": 0.0, "cpu_worst": 0.0, "late": 0.0, "muted": false}, nil
}

// EnterDFU implements box.Box. The fake only records the request.
func (f *Box) EnterDFU() error {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.dfu {
		return errors.New("already in DFU")
	}
	f.dfu = true
	return nil
}
