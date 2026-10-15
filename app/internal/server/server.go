// Package server is the HTTP side of the app: the page and the four API routes.
package server

import (
	"context"
	"encoding/json"
	"errors"
	"io/fs"
	"log"
	"net"
	"net/http"
	"strings"
	"sync"
	"time"

	"cubevox.app/internal/box"
	"cubevox.app/internal/life"
	"cubevox.app/internal/update"
)

// PushNameLayout is the record name format: the push time, to the minute.
const PushNameLayout = "2006-01-02 15:04"

const maxBody = 4096

// Config wires the server to its collaborators.
type Config struct {
	Box     box.Box
	Fixture box.Hello // card list and layout shown when no box answers
	App     string    // app version
	Web     fs.FS     // the page, rooted at index.html
	Idle    *life.Idle
	// OnPageOpen runs once, on the first heartbeat, to open the serial link.
	OnPageOpen func()
	Now        func() time.Time
	// CheckUpdate fetches the latest release; RunUpdate performs the update and reports each
	// step by name. Either nil turns the update feature off.
	CheckUpdate func(ctx context.Context, current, fw string) (update.Info, error)
	RunUpdate   func(ctx context.Context, info update.Info, step func(string)) error
}

// Limits are the budget lines the page draws its meter against.
type Limits struct {
	Mean  int `json:"mean"`
	Worst int `json:"worst"`
	Fixed int `json:"fixed"`
}

// UpdateState is the update progress the bottom bar shows.
type UpdateState struct {
	Version   string `json:"version"`
	Available bool   `json:"available"`
	Step      string `json:"step"`
	Error     string `json:"error"`
}

// State is the reply of GET /api/state.
type State struct {
	Connected bool        `json:"connected"`
	FW        string      `json:"fw"`
	App       string      `json:"app"`
	Cards     []box.Card  `json:"cards"`
	Layout    box.Layout  `json:"layout"`
	Limits    Limits      `json:"limits"`
	Update    UpdateState `json:"update"`
}

// Server holds the mutable update state; everything else is read from the box per request.
type Server struct {
	cfg Config

	mu       sync.Mutex
	update   UpdateState // step and run error; version and available are filled per request
	release  *update.Info
	checkErr string
	running  bool
}

// New returns a Server with an idle update state.
func New(cfg Config) *Server {
	if cfg.Now == nil {
		cfg.Now = time.Now
	}
	return &Server{cfg: cfg, update: UpdateState{Step: "idle"}}
}

// Handler returns the routes behind the local-origin guard.
func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /api/state", s.handleState)
	mux.HandleFunc("POST /api/layout", s.handleLayout)
	mux.HandleFunc("POST /api/update", s.handleUpdate)
	mux.HandleFunc("POST /api/heartbeat", s.handleHeartbeat)
	mux.Handle("GET /", http.FileServerFS(s.cfg.Web))
	return localOnly(mux)
}

// localOnly refuses requests whose Host or Origin is not a loopback name. A web page on another
// site could otherwise reach this server through DNS rebinding or a cross-site POST and push a
// layout to the box.
func localOnly(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if !loopbackHost(r.Host) {
			http.Error(w, "bad host", http.StatusForbidden)
			return
		}
		if o := r.Header.Get("Origin"); o != "" && !loopbackOrigin(o) {
			http.Error(w, "bad origin", http.StatusForbidden)
			return
		}
		next.ServeHTTP(w, r)
	})
}

func loopbackHost(hostport string) bool {
	host, _, err := net.SplitHostPort(hostport)
	if err != nil {
		host = hostport
	}
	return host == "127.0.0.1" || host == "localhost" || host == "[::1]" || host == "::1"
}

func loopbackOrigin(origin string) bool {
	rest, ok := strings.CutPrefix(origin, "http://")
	return ok && loopbackHost(rest)
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	if err := json.NewEncoder(w).Encode(v); err != nil {
		log.Printf("write reply: %v", err)
	}
}

func (s *Server) handleState(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, s.state())
}

func (s *Server) state() State {
	st := State{
		App:    s.cfg.App,
		Cards:  s.cfg.Fixture.Cards,
		Layout: s.cfg.Fixture.Layout,
		Limits: Limits{Mean: box.BudgetMean, Worst: box.BudgetWorst, Fixed: box.FixedCost},
	}
	hello, err := s.cfg.Box.Hello()
	switch {
	case err == nil:
		st.Connected, st.FW, st.Cards, st.Layout = true, hello.FW, hello.Cards, s.liveLayout(hello)
	case !errors.Is(err, box.ErrNotConnected):
		log.Printf("state: %v", err)
	}
	st.Update = s.updateState(st.FW)
	return st
}

// updateState merges the run progress with the release fetched at page open. The behind flags
// are recomputed against the live firmware version because the box may connect after the fetch.
// An update that failed stays available so the page keeps offering Retry while the box sits in
// DFU and reports no firmware version.
func (s *Server) updateState(fw string) UpdateState {
	s.mu.Lock()
	defer s.mu.Unlock()
	u := s.update
	if u.Error == "" {
		u.Error = s.checkErr
	}
	if s.release != nil {
		info := s.release.For(s.cfg.App, fw)
		u.Version = info.Tag
		u.Available = info.Available || s.update.Error != ""
	}
	return u
}

// liveLayout reads the layout from the box, since hello's copy is from when the port opened.
func (s *Server) liveLayout(hello box.Hello) box.Layout {
	l, err := s.cfg.Box.GetLayout()
	if err != nil {
		log.Printf("state get_layout: %v", err)
		return hello.Layout
	}
	return l
}

type layoutRequest struct {
	Slots []string `json:"slots"`
}

type layoutReply struct {
	OK     bool        `json:"ok"`
	Layout *box.Layout `json:"layout,omitempty"`
	Error  string      `json:"error,omitempty"`
}

func (s *Server) handleLayout(w http.ResponseWriter, r *http.Request) {
	var req layoutRequest
	body := http.MaxBytesReader(w, r.Body, maxBody)
	if err := json.NewDecoder(body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, layoutReply{Error: "bad request: " + err.Error()})
		return
	}
	layout, err := s.push(req.Slots)
	if err != nil {
		writeJSON(w, http.StatusOK, layoutReply{Error: boxText(err)})
		return
	}
	writeJSON(w, http.StatusOK, layoutReply{OK: true, Layout: &layout})
}

// push sends the layout and reads it back, so the page shows what the box holds.
func (s *Server) push(slots []string) (box.Layout, error) {
	name := s.cfg.Now().Format(PushNameLayout)
	if err := s.cfg.Box.SetLayout(name, slots); err != nil {
		return box.Layout{}, err
	}
	l, err := s.cfg.Box.GetLayout()
	if err != nil {
		return box.Layout{}, err
	}
	return l, nil
}

// boxText is the text the page shows: the box's own words for a refusal, else our error.
func boxText(err error) string {
	var remote *box.RemoteError
	if errors.As(err, &remote) {
		return remote.Text
	}
	log.Printf("layout push: %v", err)
	if errors.Is(err, box.ErrNotConnected) {
		return "Connect the box"
	}
	return err.Error()
}

type updateReply struct {
	OK    bool   `json:"ok"`
	Error string `json:"error,omitempty"`
}

// handleUpdate starts the update in the background; progress shows in state.update.step. It is
// also the page's Retry, since Run resumes from the first incomplete step.
func (s *Server) handleUpdate(w http.ResponseWriter, r *http.Request) {
	if err := s.startUpdate(); err != nil {
		writeJSON(w, http.StatusOK, updateReply{Error: err.Error()})
		return
	}
	writeJSON(w, http.StatusOK, updateReply{OK: true})
}

func (s *Server) startUpdate() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	switch {
	case s.cfg.RunUpdate == nil:
		return errors.New("update unavailable")
	case s.running:
		return errors.New("update running")
	case s.release == nil:
		return errors.New("no release information yet")
	}
	s.running = true
	s.update = UpdateState{Step: update.StepBackup}
	go s.runUpdate(*s.release)
	return nil
}

func (s *Server) runUpdate(info update.Info) {
	err := s.cfg.RunUpdate(context.Background(), info, s.setStep)
	s.mu.Lock()
	defer s.mu.Unlock()
	s.running = false
	if err != nil {
		log.Printf("update: %v", err)
		s.update.Error = err.Error()
	}
}

func (s *Server) setStep(name string) {
	s.mu.Lock()
	s.update.Step = name
	s.mu.Unlock()
}

// checkRelease fetches the latest release once. A network failure is shown as an error text
// and leaves the update unavailable; it never blocks the rest of the page.
func (s *Server) checkRelease() {
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	info, err := s.cfg.CheckUpdate(ctx, s.cfg.App, "")
	s.mu.Lock()
	defer s.mu.Unlock()
	if err != nil {
		log.Printf("update check: %v", err)
		s.checkErr = "Could not check for updates: " + err.Error()
		return
	}
	s.release, s.checkErr = &info, ""
}

func (s *Server) handleHeartbeat(w http.ResponseWriter, r *http.Request) {
	if s.cfg.Idle.Beat() {
		if s.cfg.OnPageOpen != nil {
			s.cfg.OnPageOpen()
		}
		if s.cfg.CheckUpdate != nil {
			go s.checkRelease()
		}
	}
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}
