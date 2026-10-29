package shclw3

import (
	"errors"
	"testing"

	shcl2 "github.com/jim-collier/shcl/source/go/v2"
	"github.com/jim-collier/shclwrap/source/go/shclw"
)

// Test ID: dejef
// TestCheckReadsRepeated: each instance of a repeated key is read back
// against 2.0, so one shcl split, as it did before a010fdf8, is refused on
// its own line (2026102816332481).
func TestCheckReadsRepeated(t *testing.T) {
	old := shcl2.Parse("row: idx=2, title=\"a, b\"\nrow: idx=3\nt:\n\tx: 1\nt:\n\tx: 2\n")
	good := "row: [idx=2, 'title=\"a, b\"']\nrow: idx=3\nt:\n\tx: 1\nt:\n\tx: 2\n"
	if lost, err := checkReads(old, good, false); err != nil || len(lost) != 0 {
		t.Fatalf("good: lost %v, err %v", lost, err)
	}
	for _, c := range []struct {
		up   string
		line int
	}{
		{"row: [idx=2, 'title=\"a', 'b\"']\nrow: idx=3\nt:\n\tx: 1\nt:\n\tx: 2\n", 1},
		{"row: [idx=2, 'title=\"a, b\"']\nrow: idx=4\nt:\n\tx: 1\nt:\n\tx: 2\n", 2},
		{"row: [idx=2, 'title=\"a, b\"']\nrow: idx=3\nt:\n\tx: 1\nt:\n\tx: 3\n", 6},
	} {
		_, err := checkReads(old, c.up, false)
		var e *shclw.Error
		if !errors.As(err, &e) || e.Kind != shclw.ErrRefused || len(e.Diags) != 1 || e.Diags[0].Line != c.line {
			t.Errorf("%q: want refused on line %d, got %v", c.up, c.line, err)
		}
	}
	// An instance 3.0 reads as empty is lost, as a single key's would be.
	lost, err := checkReads(old, "row: [idx=2, 'title=\"a, b\"']\nrow:\nt:\n\tx: 1\nt:\n\tx: 2\n", false)
	if err != nil || len(lost) != 1 || lost[0] != 2 {
		t.Errorf("empty instance: lost %v, err %v", lost, err)
	}
}
