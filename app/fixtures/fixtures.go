// Package fixtures embeds the firmware's real hello reply for the default layout.
package fixtures

import (
	_ "embed"
	"encoding/json"
	"fmt"

	"cubevox.app/internal/box"
)

//go:embed hello.json
var helloJSON []byte

// Hello decodes the embedded hello reply.
func Hello() (box.Hello, error) {
	var h box.Hello
	if err := json.Unmarshal(helloJSON, &h); err != nil {
		return box.Hello{}, fmt.Errorf("decode fixtures/hello.json: %w", err)
	}
	return h, nil
}
