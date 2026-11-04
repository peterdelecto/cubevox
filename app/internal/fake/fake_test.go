package fake

import (
	"errors"
	"strings"
	"testing"

	"cubevox.app/fixtures"
	"cubevox.app/internal/box"
)

func newFake(t *testing.T) *Box {
	t.Helper()
	h, err := fixtures.Hello()
	if err != nil {
		t.Fatal(err)
	}
	return New(h)
}

func TestDefaultLayoutIsValid(t *testing.T) {
	f := newFake(t)
	h, _ := f.Hello()
	if err := f.SetLayout("Default", h.Layout.Slots); err != nil {
		t.Fatalf("fixture's own layout rejected: %v", err)
	}
}

func TestValidateRules(t *testing.T) {
	f := newFake(t)
	empty8 := []string{"empty", "empty", "empty", "empty", "empty", "empty", "empty", "empty"}
	with := func(over ...string) []string {
		s := append([]string(nil), empty8...)
		copy(s, over)
		return s
	}
	cases := []struct {
		name  string
		label string
		slots []string
		want  string
	}{
		{"all empty is fine", "x", empty8, ""},
		{"empty repeats", "x", with("gate", "empty", "slapback"), ""},
		{"too few slots", "x", empty8[:7], "slots needs 8 card ids"},
		{"too many slots", "x", append(append([]string(nil), empty8...), "empty"), "slots needs 8 card ids"},
		{"unknown id", "x", with("gate", "empty", "foo"), `unknown card "foo" in slot 3`},
		{"same card twice", "x", with("slapback", "empty", "slapback"), "same card twice: slapback"},
		{"two reverbs", "x", with("reverb_spring", "reverb_chasm"), "two reverb cards"},
		{"empty name", "", empty8, "name must be 1 to 31 printable ASCII characters"},
		{"long name", strings.Repeat("a", 32), empty8, "name must be 1 to 31 printable ASCII characters"},
		{"name 31 ok", strings.Repeat("a", 31), empty8, ""},
		{"non-ASCII name", "café", empty8, "name must be 1 to 31 printable ASCII characters"},
		{"control char name", "a\nb", empty8, "name must be 1 to 31 printable ASCII characters"},
		// The fixed term and the eight cards fit both lines.
		{"under budget", "x", []string{"input_gate", "autotune", "octave", "unison", "slapback", "distortion", "gate", "reverb_chasm"}, ""},
	}
	for _, c := range cases {
		err := Validate(f.hello.Cards, c.label, c.slots)
		got := ""
		if err != nil {
			got = err.Error()
		}
		if got != c.want {
			t.Errorf("%s: got %q, want %q", c.name, got, c.want)
		}
	}
}

func TestBudgetLines(t *testing.T) {
	left := func(line int) int { return line - box.FixedCost } // what a card may add to the fixed term
	cards := []box.Card{
		{ID: "empty"},
		{ID: "big_mean", Cost: box.Cost{Avg: left(box.BudgetMean) + 1, Worst: 10}},  // the fixed term makes 76 > 75
		{ID: "ok_mean", Cost: box.Cost{Avg: left(box.BudgetMean), Worst: 10}},       // 75 exactly
		{ID: "big_worst", Cost: box.Cost{Avg: 10, Worst: left(box.BudgetWorst) + 1}}, // worst 96 > 95
		{ID: "ok_worst", Cost: box.Cost{Avg: 10, Worst: left(box.BudgetWorst)}},      // worst 95 exactly
	}
	pad := func(id string) []string {
		return []string{id, "empty", "empty", "empty", "empty", "empty", "empty", "empty"}
	}
	for _, c := range []struct{ id, want string }{
		{"big_mean", "over CPU budget (mean 76 %)"},
		{"ok_mean", ""},
		{"big_worst", "over CPU budget (worst 96 %)"},
		{"ok_worst", ""},
	} {
		err := Validate(cards, "x", pad(c.id))
		got := ""
		if err != nil {
			got = err.Error()
		}
		if got != c.want {
			t.Errorf("%s: got %q, want %q", c.id, got, c.want)
		}
	}
}

func TestSetLayoutRefusalIsRemoteError(t *testing.T) {
	f := newFake(t)
	err := f.SetLayout("x", []string{"nope", "empty", "empty", "empty", "empty", "empty", "empty", "empty"})
	var remote *box.RemoteError
	if !errors.As(err, &remote) || remote.Text != `unknown card "nope" in slot 1` {
		t.Fatalf("got %v", err)
	}
}

func TestSetThenGet(t *testing.T) {
	f := newFake(t)
	slots := []string{"gate", "empty", "empty", "empty", "empty", "empty", "empty", "reverb_spring"}
	if err := f.SetLayout("2026-10-15 14:02", slots); err != nil {
		t.Fatal(err)
	}
	l, _ := f.GetLayout()
	if l.Name != "2026-10-15 14:02" || strings.Join(l.Slots, ",") != strings.Join(slots, ",") {
		t.Fatalf("read-back %+v", l)
	}
}
