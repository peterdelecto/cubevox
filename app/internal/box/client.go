package box

import (
	"bufio"
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"sync"
	"time"
)

// DefaultTimeout bounds one request once the link is established.
const DefaultTimeout = 3 * time.Second

// Client speaks the JSON line protocol over one open connection. The port also carries boot
// report and log text, so the reader keeps only lines that start with "{" and routes each to its
// request by id. Client implements Box.
type Client struct {
	// Timeout bounds each request; zero means DefaultTimeout.
	Timeout time.Duration

	rw   io.ReadWriteCloser
	wmu  sync.Mutex // serialises writes
	mu   sync.Mutex // guards the fields below
	next int
	wait map[int]chan []byte
	err  error // set when the reader stops
	done chan struct{}
}

// NewClient starts the reader on rw. Close the Client to release rw.
func NewClient(rw io.ReadWriteCloser) *Client {
	c := &Client{rw: rw, wait: map[int]chan []byte{}, done: make(chan struct{})}
	go c.readLoop()
	return c
}

// Done is closed when the connection fails or is closed.
func (c *Client) Done() <-chan struct{} { return c.done }

// Close closes the connection and fails every pending request.
func (c *Client) Close() error {
	err := c.rw.Close()
	c.fail(errors.New("link closed"))
	return err
}

func (c *Client) readLoop() {
	r := bufio.NewReaderSize(c.rw, 8192)
	for {
		line, err := r.ReadBytes('\n')
		if len(line) > 0 {
			c.dispatch(bytes.TrimRight(line, "\r\n"))
		}
		if err != nil {
			c.fail(fmt.Errorf("read: %w", err))
			return
		}
	}
}

// dispatch hands a JSON line to the request that owns its id. Other lines are log text or noise.
func (c *Client) dispatch(line []byte) {
	if len(line) == 0 || line[0] != '{' {
		return
	}
	var head struct {
		ID int `json:"id"`
	}
	if json.Unmarshal(line, &head) != nil {
		return
	}
	c.mu.Lock()
	ch := c.wait[head.ID]
	delete(c.wait, head.ID)
	c.mu.Unlock()
	if ch != nil {
		ch <- append([]byte(nil), line...)
	}
}

// fail records the first error and wakes every waiter.
func (c *Client) fail(err error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.err != nil {
		return
	}
	c.err = err
	close(c.done)
}

func (c *Client) register() (int, chan []byte, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.err != nil {
		return 0, nil, c.err
	}
	c.next++
	ch := make(chan []byte, 1)
	c.wait[c.next] = ch
	return c.next, ch, nil
}

func (c *Client) forget(id int) {
	c.mu.Lock()
	delete(c.wait, id)
	c.mu.Unlock()
}

// call sends one request and decodes the reply into out (nil to ignore the body). A reply with
// ok false becomes a *RemoteError carrying the box's text.
func (c *Client) call(cmd string, fields map[string]any, out any) error {
	id, ch, err := c.register()
	if err != nil {
		return err
	}
	req := map[string]any{"id": id, "cmd": cmd}
	for k, v := range fields {
		req[k] = v
	}
	data, err := json.Marshal(req)
	if err != nil {
		c.forget(id)
		return fmt.Errorf("encode %s: %w", cmd, err)
	}
	c.wmu.Lock()
	_, err = c.rw.Write(append(data, '\n'))
	c.wmu.Unlock()
	if err != nil {
		c.forget(id)
		c.fail(fmt.Errorf("write: %w", err))
		return fmt.Errorf("write %s: %w", cmd, err)
	}
	return c.await(cmd, id, ch, out)
}

func (c *Client) await(cmd string, id int, ch chan []byte, out any) error {
	timeout := c.Timeout
	if timeout == 0 {
		timeout = DefaultTimeout
	}
	timer := time.NewTimer(timeout)
	defer timer.Stop()
	select {
	case line := <-ch:
		return decodeReply(cmd, line, out)
	case <-c.done:
		c.forget(id)
		c.mu.Lock()
		defer c.mu.Unlock()
		return c.err
	case <-timer.C:
		c.forget(id)
		return fmt.Errorf("%s: no reply in %v", cmd, timeout)
	}
}

func decodeReply(cmd string, line []byte, out any) error {
	var head struct {
		OK    bool   `json:"ok"`
		Error string `json:"error"`
	}
	if err := json.Unmarshal(line, &head); err != nil {
		return fmt.Errorf("%s reply: %w", cmd, err)
	}
	if !head.OK {
		return &RemoteError{Text: head.Error}
	}
	if out == nil {
		return nil
	}
	if err := json.Unmarshal(line, out); err != nil {
		return fmt.Errorf("%s reply: %w", cmd, err)
	}
	return nil
}

// Hello implements Box.
func (c *Client) Hello() (Hello, error) {
	var h Hello
	err := c.call("hello", nil, &h)
	return h, wrap("hello", err)
}

// GetLayout implements Box.
func (c *Client) GetLayout() (Layout, error) {
	var r struct {
		Layout Layout `json:"layout"`
	}
	err := c.call("get_layout", nil, &r)
	return r.Layout, wrap("get_layout", err)
}

// SetLayout implements Box. A refusal comes back as a *RemoteError with the box's text.
func (c *Client) SetLayout(name string, slots []string) error {
	err := c.call("set_layout", map[string]any{"name": name, "slots": slots}, nil)
	var remote *RemoteError
	if errors.As(err, &remote) {
		return err
	}
	return wrap("set_layout", err)
}

// Status implements Box.
func (c *Client) Status() (Status, error) {
	var s Status
	err := c.call("status", nil, &s)
	return s, wrap("status", err)
}

// EnterDFU implements Box.
func (c *Client) EnterDFU() error {
	return wrap("enter_dfu", c.call("enter_dfu", nil, nil))
}
