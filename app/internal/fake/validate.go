package fake

import (
	"errors"
	"fmt"

	"cubevox.app/internal/box"
)

const emptyID = "empty"

// Validate applies the firmware's set_layout rules (core/protocol.cpp setLayoutProblem and
// resolveLayout, core/layout.cpp layoutValid) in the same order, with the same wording.
func Validate(cards []box.Card, name string, slots []string) error {
	if !printableName(name) {
		return errors.New("name must be 1 to 31 printable ASCII characters")
	}
	if len(slots) != box.Slots {
		return errors.New("slots needs 8 card ids")
	}
	byID := make(map[string]box.Card, len(cards))
	for _, c := range cards {
		byID[c.ID] = c
	}
	for i, id := range slots {
		if _, ok := byID[id]; !ok {
			return fmt.Errorf("unknown card %q in slot %d", id, i+1)
		}
	}
	return checkBudget(byID, slots)
}

func printableName(s string) bool {
	if len(s) < 1 || len(s) > 31 {
		return false
	}
	for i := 0; i < len(s); i++ {
		if s[i] < 0x20 || s[i] > 0x7e {
			return false
		}
	}
	return true
}

// checkBudget runs layoutValid: duplicates stop at the first repeat, then reverbs, then the sums.
func checkBudget(byID map[string]box.Card, slots []string) error {
	mean, worst, reverbs := box.FixedCost, box.FixedCost, 0
	seen := map[string]bool{}
	for _, id := range slots {
		if id != emptyID {
			if seen[id] {
				return fmt.Errorf("same card twice: %s", id)
			}
			seen[id] = true
		}
		c := byID[id]
		mean += c.Cost.Avg
		worst += c.Cost.Worst
		if c.Group == "reverb" {
			reverbs++
		}
	}
	if reverbs > 1 {
		return errors.New("two reverb cards")
	}
	if mean > box.BudgetMean {
		return fmt.Errorf("over CPU budget (mean %d %%)", mean)
	}
	if worst > box.BudgetWorst {
		return fmt.Errorf("over CPU budget (worst %d %%)", worst)
	}
	return nil
}
