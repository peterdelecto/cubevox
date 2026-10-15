package update

import "testing"

func TestNewer(t *testing.T) {
	cases := []struct {
		tag, cur string
		want     bool
	}{
		{"v0.01.01", "v0.01.00", true},
		{"v0.01.00", "v0.01.00", false},
		{"v0.01.00", "v0.01.01", false},
		{"v0.02.00", "v0.01.99", true},
		{"v1.00.00", "v0.99.99", true},
		{"v0.10.00", "v0.9.00", true}, // numeric, not text, order
		{"v0.01.01", "dev", false},
		{"v0.01.01", "", false},
		{"latest", "v0.01.00", false},
		{"v0.01", "v0.01.00", false},
		{"v0.01.01", "0.01.00", true},
		{"v0.01.01", "v0.01.00-3-gabc1234", true}, // between tags compares as its tag
		{"v0.01.00", "v0.01.00-3-gabc1234", false},
	}
	for _, c := range cases {
		if got := Newer(c.tag, c.cur); got != c.want {
			t.Errorf("Newer(%q, %q) = %v, want %v", c.tag, c.cur, got, c.want)
		}
	}
}
