// Package box defines the cubevox box as the app sees it and the JSON line link to a real one.
package box

import (
	"errors"
	"fmt"
)

// Slots is the number of effect slots on the box.
const Slots = 8

// Budget lines the box enforces on a layout (core/costs.h kBudgetAvgPercent, kBudgetWorstPercent).
const (
	BudgetMean  = 75
	BudgetWorst = 95
)

// FixedCost is the output EQ and sample conversion share every layout pays. hello does not carry
// it, so it mirrors firmware/cubevox/src/core/costs.h kFixed (3 % mean and worst).
const FixedCost = 3

// ErrNotConnected is returned when no box answers.
var ErrNotConnected = errors.New("box not connected")

// Cost is a card's processing share in percent of the CPU budget.
type Cost struct {
	Avg      int  `json:"avg"`
	Worst    int  `json:"worst"`
	Measured bool `json:"measured"`
}

// Knob describes one pot as the box reports it.
type Knob struct {
	Name      string  `json:"name"`
	Kind      string  `json:"kind"`
	Positions int     `json:"positions"`
	Min       float64 `json:"min"`
	Max       float64 `json:"max"`
	Unit      string  `json:"unit"`
	Curve     string  `json:"curve"`
}

// Card is one registry entry from hello.
type Card struct {
	ID    string `json:"id"`
	Name  string `json:"name"`
	Group string `json:"group"`
	Cost  Cost   `json:"cost"`
	Knobs []Knob `json:"knobs"`
}

// Layout is the record name plus the eight slot card ids.
type Layout struct {
	Name  string   `json:"name"`
	Slots []string `json:"slots"`
}

// Hello is the box's reply to the hello command.
type Hello struct {
	Proto    int      `json:"proto"`
	FW       string   `json:"fw"`
	Build    string   `json:"build"`
	BenchOps []string `json:"bench_ops"`
	Cards    []Card   `json:"cards"`
	Layout   Layout   `json:"layout"`
}

// Status is the box's status reply, kept as the box sent it (cpu_mean, cpu_worst, late, ...).
type Status map[string]any

// Box is everything the app asks of a box, real or fake.
type Box interface {
	Hello() (Hello, error)
	GetLayout() (Layout, error)
	SetLayout(name string, slots []string) error
	Status() (Status, error)
	EnterDFU() error
}

// RemoteError carries the box's own error text so the page can show it unchanged.
type RemoteError struct{ Text string }

func (e *RemoteError) Error() string { return e.Text }

func wrap(op string, err error) error {
	if err == nil {
		return nil
	}
	return fmt.Errorf("%s: %w", op, err)
}
