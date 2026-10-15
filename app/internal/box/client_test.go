package box

import (
	"bufio"
	"encoding/json"
	"errors"
	"io"
	"strings"
	"testing"
	"time"
)

// pipeBox is the box side of an in-memory link: it reads requests and writes whatever a test says.
type pipeBox struct {
	r *bufio.Reader
	w io.WriteCloser
}

func newPair() (*Client, *pipeBox) {
	reqR, reqW := io.Pipe() // client writes, box reads
	repR, repW := io.Pipe() // box writes, client reads
	c := NewClient(struct {
		io.Reader
		io.Writer
		io.Closer
	}{repR, reqW, closers{reqW, repW}})
	c.Timeout = time.Second
	return c, &pipeBox{r: bufio.NewReader(reqR), w: repW}
}

type closers []io.Closer

func (cs closers) Close() error {
	for _, c := range cs {
		c.Close()
	}
	return nil
}

func (b *pipeBox) request(t *testing.T) map[string]any {
	t.Helper()
	// Runs in helper goroutines, so it reports with Errorf and returns an empty map.
	m := map[string]any{"slots": []any{}}
	line, err := b.r.ReadBytes('\n')
	if err != nil {
		t.Errorf("read request: %v", err)
		return m
	}
	if err := json.Unmarshal(line, &m); err != nil {
		t.Errorf("request %q: %v", line, err)
	}
	return m
}

func (b *pipeBox) send(s string) { io.WriteString(b.w, s) }

func TestFramingSkipsNoiseAndMatchesID(t *testing.T) {
	c, b := newPair()
	defer c.Close()
	go func() {
		req := b.request(t)
		if req["cmd"] != "get_layout" {
			t.Errorf("cmd %v", req["cmd"])
		}
		b.send("[boot] build release\r\n")
		b.send("garbage with { inside\n")
		b.send("\x00\xff\n")
		b.send(`{"id":99,"ok":true,"layout":{"name":"stale","slots":[]}}` + "\n") // other id
		b.send(`{not json` + "\n")
		b.send(`{"id":1,"ok":true,"layout":{"name":"Default","slots":["gate","empty","empty","empty","empty","empty","empty","empty"]}}` + "\r\n")
	}()
	l, err := c.GetLayout()
	if err != nil {
		t.Fatal(err)
	}
	if l.Name != "Default" || len(l.Slots) != 8 || l.Slots[0] != "gate" {
		t.Fatalf("layout %+v", l)
	}
}

func TestRepliesOutOfOrder(t *testing.T) {
	c, b := newPair()
	defer c.Close()
	go func() {
		first, second := b.request(t), b.request(t)
		b.send(`{"id":2,"ok":true,"cpu_mean":1.5}` + "\n")
		b.send(`{"id":1,"ok":true,"layout":{"name":"A","slots":[]}}` + "\n")
		_, _ = first, second
	}()
	type res struct {
		name string
		err  error
	}
	out := make(chan res, 2)
	go func() { l, err := c.GetLayout(); out <- res{"layout:" + l.Name, err} }()
	time.Sleep(50 * time.Millisecond) // order the ids: get_layout is 1
	go func() { s, err := c.Status(); out <- res{"status", err}; _ = s }()
	got := map[string]bool{}
	for i := 0; i < 2; i++ {
		r := <-out
		if r.err != nil {
			t.Fatal(r.err)
		}
		got[r.name] = true
	}
	if !got["layout:A"] || !got["status"] {
		t.Fatalf("got %v", got)
	}
}

func TestSetLayoutErrorText(t *testing.T) {
	c, b := newPair()
	defer c.Close()
	go func() {
		req := b.request(t)
		if req["name"] != "n" || len(req["slots"].([]any)) != 8 {
			t.Errorf("request %v", req)
		}
		b.send(`{"id":1,"ok":false,"error":"over CPU budget (worst 97 %)"}` + "\n")
	}()
	err := c.SetLayout("n", strings.Fields("a b c d e f g h"))
	var remote *RemoteError
	if !errors.As(err, &remote) || remote.Text != "over CPU budget (worst 97 %)" {
		t.Fatalf("err %v", err)
	}
}

func TestTimeout(t *testing.T) {
	c, b := newPair()
	defer c.Close()
	c.Timeout = 50 * time.Millisecond
	go b.request(t)
	if _, err := c.Status(); err == nil || !strings.Contains(err.Error(), "no reply") {
		t.Fatalf("err %v", err)
	}
}

func TestReadErrorFailsPending(t *testing.T) {
	c, b := newPair()
	go func() { b.request(t); b.w.Close() }()
	if _, err := c.Hello(); err == nil {
		t.Fatal("expected error when the link drops")
	}
	select {
	case <-c.Done():
	case <-time.After(time.Second):
		t.Fatal("Done not closed")
	}
	if _, err := c.Status(); err == nil {
		t.Fatal("calls after failure should error")
	}
}
