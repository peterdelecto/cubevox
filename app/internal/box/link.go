package box

import (
	"context"
	"errors"
	"fmt"
	"log"
	"path/filepath"
	"sync"
	"time"
)

// Timing of the link, from the Mac app spec.
const (
	HelloTimeout  = 2 * time.Second // a port that does not answer hello in this time is skipped
	RetryInterval = 2 * time.Second // between connection attempts
	PollInterval  = time.Second     // status poll that doubles as the liveness check
	portPattern   = "/dev/cu.usbmodem*"
)

// dialFunc finds a box and returns a live client with its hello.
type dialFunc func() (*Client, Hello, error)

// Link keeps a connection to a real box while it is running: it finds the port, polls status
// once a second, and reconnects every 2 s after an error. It implements Box; every call fails
// with ErrNotConnected while no box is attached.
type Link struct {
	dial dialFunc
	poll time.Duration
	wait time.Duration

	mu     sync.Mutex
	client *Client
	hello  Hello
	status Status
	cancel context.CancelFunc
	done   chan struct{}
}

// NewLink returns a stopped link that discovers /dev/cu.usbmodem* ports.
func NewLink() *Link {
	return &Link{dial: Discover, poll: PollInterval, wait: RetryInterval}
}

// Start opens the link in the background. It does nothing if already started.
func (l *Link) Start() {
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.cancel != nil {
		return
	}
	ctx, cancel := context.WithCancel(context.Background())
	l.cancel = cancel
	l.done = make(chan struct{})
	go l.run(ctx, l.done)
}

// Stop closes the port and ends the background loop.
func (l *Link) Stop() {
	l.mu.Lock()
	cancel, done := l.cancel, l.done
	l.cancel = nil
	l.mu.Unlock()
	if cancel == nil {
		return
	}
	cancel()
	<-done
}

func (l *Link) run(ctx context.Context, done chan struct{}) {
	defer close(done)
	for ctx.Err() == nil {
		c, h, err := l.dial()
		if err != nil {
			if !sleep(ctx, l.wait) {
				return
			}
			continue
		}
		l.attach(c, h)
		err = l.watch(ctx, c)
		l.detach(c)
		if ctx.Err() != nil {
			return
		}
		log.Printf("box link lost: %v", err)
		if !sleep(ctx, l.wait) {
			return
		}
	}
}

// watch polls status until the connection fails or ctx ends.
func (l *Link) watch(ctx context.Context, c *Client) error {
	tick := time.NewTicker(l.poll)
	defer tick.Stop()
	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-c.Done():
			return errors.New("connection closed")
		case <-tick.C:
			s, err := c.Status()
			if err != nil {
				return err
			}
			l.mu.Lock()
			l.status = s
			l.mu.Unlock()
		}
	}
}

func (l *Link) attach(c *Client, h Hello) {
	l.mu.Lock()
	l.client, l.hello, l.status = c, h, nil
	l.mu.Unlock()
}

func (l *Link) detach(c *Client) {
	l.mu.Lock()
	l.client = nil
	l.mu.Unlock()
	if err := c.Close(); err != nil {
		log.Printf("close box port: %v", err)
	}
}

func (l *Link) current() (*Client, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.client == nil {
		return nil, ErrNotConnected
	}
	return l.client, nil
}

func sleep(ctx context.Context, d time.Duration) bool {
	t := time.NewTimer(d)
	defer t.Stop()
	select {
	case <-ctx.Done():
		return false
	case <-t.C:
		return true
	}
}

// Hello returns the reply captured when the port was opened.
func (l *Link) Hello() (Hello, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.client == nil {
		return Hello{}, ErrNotConnected
	}
	return l.hello, nil
}

// GetLayout implements Box.
func (l *Link) GetLayout() (Layout, error) {
	c, err := l.current()
	if err != nil {
		return Layout{}, err
	}
	return c.GetLayout()
}

// SetLayout implements Box.
func (l *Link) SetLayout(name string, slots []string) error {
	c, err := l.current()
	if err != nil {
		return err
	}
	return c.SetLayout(name, slots)
}

// Status returns the newest polled status.
func (l *Link) Status() (Status, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.client == nil {
		return nil, ErrNotConnected
	}
	return l.status, nil
}

// EnterDFU implements Box.
func (l *Link) EnterDFU() error {
	c, err := l.current()
	if err != nil {
		return err
	}
	return c.EnterDFU()
}

// Discover opens each /dev/cu.usbmodem* port in turn and keeps the first that answers hello.
func Discover() (*Client, Hello, error) {
	ports, err := filepath.Glob(portPattern)
	if err != nil {
		return nil, Hello{}, fmt.Errorf("list ports: %w", err)
	}
	if len(ports) == 0 {
		return nil, Hello{}, ErrNotConnected
	}
	for _, p := range ports {
		if c, h, err := tryPort(p); err == nil {
			return c, h, nil
		}
	}
	return nil, Hello{}, ErrNotConnected
}

func tryPort(path string) (*Client, Hello, error) {
	rw, err := openSerial(path)
	if err != nil {
		log.Printf("skip %s: %v", path, err)
		return nil, Hello{}, err
	}
	c := NewClient(rw)
	c.Timeout = HelloTimeout
	h, err := c.Hello()
	c.Timeout = DefaultTimeout
	if err != nil {
		if cerr := c.Close(); cerr != nil {
			log.Printf("close %s: %v", path, cerr)
		}
		return nil, Hello{}, err
	}
	return c, h, nil
}
