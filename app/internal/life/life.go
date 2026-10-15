// Package life decides when the app has been idle long enough to exit.
package life

import (
	"sync"
	"time"
)

// IdleLimit is how long the app waits after the last page heartbeat, and the minimum run time.
const IdleLimit = 60 * time.Second

// Idle tracks heartbeats against an injected clock. A page that never opens still lets the app
// exit, because the start time counts as the first beat.
type Idle struct {
	now   func() time.Time
	limit time.Duration

	mu    sync.Mutex
	start time.Time
	last  time.Time
	seen  bool
}

// NewIdle starts the timer. A nil clock uses time.Now.
func NewIdle(now func() time.Time, limit time.Duration) *Idle {
	if now == nil {
		now = time.Now
	}
	t := now()
	return &Idle{now: now, limit: limit, start: t, last: t}
}

// Beat records a heartbeat and reports whether it was the first one.
func (i *Idle) Beat() (first bool) {
	i.mu.Lock()
	defer i.mu.Unlock()
	first = !i.seen
	i.seen = true
	i.last = i.now()
	return first
}

// Expired is true once the limit has passed since the last beat and since start.
func (i *Idle) Expired() bool {
	i.mu.Lock()
	defer i.mu.Unlock()
	now := i.now()
	return now.Sub(i.last) >= i.limit && now.Sub(i.start) >= i.limit
}
