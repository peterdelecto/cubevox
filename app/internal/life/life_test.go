package life

import (
	"testing"
	"time"
)

type clock struct{ t time.Time }

func (c *clock) now() time.Time { return c.t }

func TestIdle(t *testing.T) {
	c := &clock{t: time.Unix(1000, 0)}
	i := NewIdle(c.now, IdleLimit)

	c.t = c.t.Add(59 * time.Second)
	if i.Expired() {
		t.Fatal("expired before the limit with no page")
	}
	c.t = c.t.Add(time.Second)
	if !i.Expired() {
		t.Fatal("no page: should expire 60 s after start")
	}

	if !i.Beat() {
		t.Fatal("first Beat should report first")
	}
	if i.Beat() {
		t.Fatal("second Beat should not report first")
	}
	c.t = c.t.Add(59 * time.Second)
	if i.Expired() {
		t.Fatal("expired 59 s after a beat")
	}
	i.Beat()
	c.t = c.t.Add(59 * time.Second)
	if i.Expired() {
		t.Fatal("beat did not reset the timer")
	}
	c.t = c.t.Add(time.Second)
	if !i.Expired() {
		t.Fatal("should expire 60 s after the last beat")
	}
}

func TestIdleMinimumRunTime(t *testing.T) {
	c := &clock{t: time.Unix(0, 0)}
	i := NewIdle(c.now, IdleLimit)
	i.last = c.t.Add(-time.Hour) // a stale beat must not beat the start rule
	c.t = c.t.Add(30 * time.Second)
	if i.Expired() {
		t.Fatal("expired before 60 s from start")
	}
}
