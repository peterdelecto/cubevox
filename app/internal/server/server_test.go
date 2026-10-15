package server

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"testing/fstest"
	"time"

	"cubevox.app/fixtures"
	"cubevox.app/internal/box"
	"cubevox.app/internal/fake"
	"cubevox.app/internal/life"
	"cubevox.app/internal/update"
)

type offline struct{ box.Box }

func (offline) Hello() (box.Hello, error)        { return box.Hello{}, box.ErrNotConnected }
func (offline) GetLayout() (box.Layout, error)   { return box.Layout{}, box.ErrNotConnected }
func (offline) SetLayout(string, []string) error { return box.ErrNotConnected }

func newServer(t *testing.T, b box.Box, onOpen func()) *httptest.Server {
	t.Helper()
	h, err := fixtures.Hello()
	if err != nil {
		t.Fatal(err)
	}
	if b == nil {
		b = fake.New(h)
	}
	web := fstest.MapFS{"index.html": {Data: []byte("cubevox")}}
	s := New(Config{
		Box: b, Fixture: h, App: "v9.99.99", Web: web,
		Idle:       life.NewIdle(nil, life.IdleLimit),
		OnPageOpen: onOpen,
		Now:        func() time.Time { return time.Date(2026, 10, 15, 14, 2, 33, 0, time.UTC) },
	})
	ts := httptest.NewServer(s.Handler())
	t.Cleanup(ts.Close)
	return ts
}

func post(t *testing.T, url, body string) map[string]any {
	t.Helper()
	resp, err := http.Post(url, "application/json", strings.NewReader(body))
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	var m map[string]any
	if err := json.NewDecoder(resp.Body).Decode(&m); err != nil {
		t.Fatal(err)
	}
	return m
}

func getState(t *testing.T, ts *httptest.Server) State {
	t.Helper()
	resp, err := http.Get(ts.URL + "/api/state")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	var st State
	if err := json.NewDecoder(resp.Body).Decode(&st); err != nil {
		t.Fatal(err)
	}
	return st
}

func TestState(t *testing.T) {
	st := getState(t, newServer(t, nil, nil))
	if !st.Connected || st.App != "v9.99.99" || len(st.Cards) != 10 || len(st.Layout.Slots) != 8 {
		t.Fatalf("state %+v", st)
	}
	if st.Limits != (Limits{Mean: 75, Worst: 95, Fixed: 3}) {
		t.Fatalf("limits %+v", st.Limits)
	}
	if st.Update != (UpdateState{Step: "idle"}) {
		t.Fatalf("update %+v", st.Update)
	}
}

func TestStateWithoutBoxUsesFixture(t *testing.T) {
	st := getState(t, newServer(t, offline{}, nil))
	if st.Connected || st.FW != "" || len(st.Cards) != 10 || len(st.Layout.Slots) != 8 {
		t.Fatalf("state %+v", st)
	}
}

func TestLayoutPushAndReadBack(t *testing.T) {
	ts := newServer(t, nil, nil)
	slots := `["gate","empty","empty","empty","empty","empty","empty","reverb_chasm"]`
	m := post(t, ts.URL+"/api/layout", `{"slots":`+slots+`}`)
	if m["ok"] != true {
		t.Fatalf("reply %v", m)
	}
	l := m["layout"].(map[string]any)
	if l["name"] != "2026-10-15 14:02" || l["slots"].([]any)[7] != "reverb_chasm" {
		t.Fatalf("layout %v", l)
	}
	if st := getState(t, ts); st.Layout.Slots[0] != "gate" {
		t.Fatalf("state did not follow the push: %+v", st.Layout)
	}
}

func TestLayoutBoxErrorPassesThrough(t *testing.T) {
	ts := newServer(t, nil, nil)
	m := post(t, ts.URL+"/api/layout", `{"slots":["reverb_spring","reverb_chasm","empty","empty","empty","empty","empty","empty"]}`)
	if m["ok"] != false || m["error"] != "two reverb cards" {
		t.Fatalf("reply %v", m)
	}
}

func TestLayoutWithoutBox(t *testing.T) {
	m := post(t, newServer(t, offline{}, nil).URL+"/api/layout", `{"slots":[]}`)
	if m["ok"] != false || m["error"] != "Connect the box" {
		t.Fatalf("reply %v", m)
	}
}

func TestLayoutBadBody(t *testing.T) {
	ts := newServer(t, nil, nil)
	resp, err := http.Post(ts.URL+"/api/layout", "application/json", strings.NewReader("{nope"))
	if err != nil {
		t.Fatal(err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusBadRequest {
		t.Fatalf("status %d", resp.StatusCode)
	}
}

// updateServer is a server whose release check and update run are the given fakes.
func updateServer(t *testing.T, check func(context.Context, string, string) (update.Info, error),
	run func(context.Context, update.Info, func(string)) error) *httptest.Server {
	t.Helper()
	h, err := fixtures.Hello()
	if err != nil {
		t.Fatal(err)
	}
	s := New(Config{
		Box: fake.New(h), Fixture: h, App: "v0.01.00",
		Web:         fstest.MapFS{"index.html": {Data: []byte("cubevox")}},
		Idle:        life.NewIdle(nil, life.IdleLimit),
		CheckUpdate: check, RunUpdate: run,
	})
	ts := httptest.NewServer(s.Handler())
	t.Cleanup(ts.Close)
	return ts
}

func waitState(t *testing.T, ts *httptest.Server, ok func(State) bool) State {
	t.Helper()
	for i := 0; i < 200; i++ {
		if st := getState(t, ts); ok(st) {
			return st
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatalf("state never matched: %+v", getState(t, ts))
	return State{}
}

func TestUpdateOffered(t *testing.T) {
	calls := 0
	ts := updateServer(t, func(_ context.Context, cur, fw string) (update.Info, error) {
		calls++
		return update.Info{Tag: "v0.02.00"}.For(cur, fw), nil
	}, nil)
	for i := 0; i < 3; i++ {
		post(t, ts.URL+"/api/heartbeat", "")
	}
	st := waitState(t, ts, func(s State) bool { return s.Update.Version != "" })
	if !st.Update.Available || st.Update.Version != "v0.02.00" || st.Update.Step != "idle" {
		t.Fatalf("update %+v", st.Update)
	}
	if calls != 1 {
		t.Fatalf("release checked %d times", calls)
	}
}

func TestUpdateCheckFailureIsOnlyText(t *testing.T) {
	ts := updateServer(t, func(context.Context, string, string) (update.Info, error) {
		return update.Info{}, errors.New("offline")
	}, nil)
	post(t, ts.URL+"/api/heartbeat", "")
	st := waitState(t, ts, func(s State) bool { return s.Update.Error != "" })
	if st.Update.Available || !strings.Contains(st.Update.Error, "offline") || !st.Connected {
		t.Fatalf("state %+v", st)
	}
}

func TestUpdateRunStepsErrorAndRetry(t *testing.T) {
	release := make(chan struct{})
	attempt := 0
	ts := updateServer(t,
		func(_ context.Context, cur, fw string) (update.Info, error) {
			return update.Info{Tag: "v0.02.00"}.For(cur, fw), nil
		},
		func(_ context.Context, info update.Info, step func(string)) error {
			attempt++
			step("dfu")
			<-release
			if attempt == 1 {
				return errors.New("dfu-util output")
			}
			step("done")
			return nil
		})
	post(t, ts.URL+"/api/heartbeat", "")
	waitState(t, ts, func(s State) bool { return s.Update.Version != "" })

	if m := post(t, ts.URL+"/api/update", ""); m["ok"] != true {
		t.Fatalf("reply %v", m)
	}
	waitState(t, ts, func(s State) bool { return s.Update.Step == "dfu" })
	if m := post(t, ts.URL+"/api/update", ""); m["ok"] != false || m["error"] != "update running" {
		t.Fatalf("second start while busy: %v", m)
	}

	release <- struct{}{}
	st := waitState(t, ts, func(s State) bool { return s.Update.Error != "" })
	if st.Update.Step != "dfu" || !st.Update.Available || st.Update.Error != "dfu-util output" {
		t.Fatalf("after failure %+v", st.Update)
	}

	var m map[string]any
	for i := 0; i < 100; i++ { // the run goroutine may still be unwinding
		if m = post(t, ts.URL+"/api/update", ""); m["ok"] == true {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	if m["ok"] != true {
		t.Fatalf("retry refused: %v", m)
	}
	release <- struct{}{}
	st = waitState(t, ts, func(s State) bool { return s.Update.Step == "done" })
	if st.Update.Error != "" {
		t.Fatalf("error survived the retry: %+v", st.Update)
	}
}

func TestUpdateRefusals(t *testing.T) {
	ts := updateServer(t, nil, nil)
	if m := post(t, ts.URL+"/api/update", ""); m["ok"] != false {
		t.Fatalf("update with no runner: %v", m)
	}
	ts = updateServer(t, nil, func(context.Context, update.Info, func(string)) error { return nil })
	if m := post(t, ts.URL+"/api/update", ""); m["ok"] != false {
		t.Fatalf("update before any release is known: %v", m)
	}
}

func TestHeartbeatOpensLinkOnce(t *testing.T) {
	opened := 0
	ts := newServer(t, nil, func() { opened++ })
	for i := 0; i < 3; i++ {
		if m := post(t, ts.URL+"/api/heartbeat", ""); m["ok"] != true {
			t.Fatalf("reply %v", m)
		}
	}
	if opened != 1 {
		t.Fatalf("OnPageOpen ran %d times", opened)
	}
}

func TestStaticPage(t *testing.T) {
	resp, err := http.Get(newServer(t, nil, nil).URL + "/")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != 200 || string(body) != "cubevox" {
		t.Fatalf("%d %q", resp.StatusCode, body)
	}
}

func TestForeignHostAndOriginRefused(t *testing.T) {
	ts := newServer(t, nil, nil)
	for _, c := range []struct{ host, origin string }{
		{"evil.example", ""},
		{"", "http://evil.example"},
	} {
		req, _ := http.NewRequest("GET", ts.URL+"/api/state", nil)
		if c.host != "" {
			req.Host = c.host
		}
		if c.origin != "" {
			req.Header.Set("Origin", c.origin)
		}
		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Fatal(err)
		}
		resp.Body.Close()
		if resp.StatusCode != http.StatusForbidden {
			t.Errorf("host %q origin %q: status %d", c.host, c.origin, resp.StatusCode)
		}
	}
}
