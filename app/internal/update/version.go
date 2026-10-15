// Package update checks GitHub Releases and runs the one-click firmware and app update.
package update

import (
	"strconv"
	"strings"
)

// Tag returns the release tag inside a version string: the part before the first '-'. A
// firmware built between tags reports "v0.01.02-3-gabc1234" and compares as v0.01.02.
func Tag(s string) string {
	tag, _, _ := strings.Cut(s, "-")
	return tag
}

// parse reads "vX.YY.ZZ" (the leading v is optional) into three numbers.
func parse(s string) ([3]int, bool) {
	var v [3]int
	parts := strings.Split(strings.TrimPrefix(Tag(s), "v"), ".")
	if len(parts) != 3 {
		return v, false
	}
	for i, p := range parts {
		n, err := strconv.Atoi(p)
		if err != nil || n < 0 {
			return v, false
		}
		v[i] = n
	}
	return v, true
}

// Newer reports whether release tag is a later version than current. Either side that is not a
// version (a "dev" build, an empty firmware string) is never behind, so it never offers an update.
func Newer(tag, current string) bool {
	t, ok := parse(tag)
	if !ok {
		return false
	}
	c, ok := parse(current)
	if !ok {
		return false
	}
	for i := range t {
		if t[i] != c[i] {
			return t[i] > c[i]
		}
	}
	return false
}
