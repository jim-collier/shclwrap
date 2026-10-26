// Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
// SPDX-License-Identifier: MIT

package shclw

import (
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"testing"
)

// Of several creates of one name at once, exactly one wins, and the file is
// all of the winner's text.
// Test ID: de8hv
func TestCreateRace(t *testing.T) {
	for name, create := range map[string]func(path, data string) error{
		"link":    func(path, data string) error { return CreateNew(path, data, 0o600) },
		"inplace": func(path, data string) error { return createInPlace(path, data, 0o600) },
	} {
		t.Run(name, func(t *testing.T) {
			for round := 0; round < 20; round++ {
				dir := t.TempDir()
				path := filepath.Join(dir, "a.shcl")
				const racers = 8
				errs := make([]error, racers)
				var wg sync.WaitGroup
				for i := range errs {
					wg.Add(1)
					go func(i int) {
						defer wg.Done()
						errs[i] = create(path, strings.Repeat("v: "+strconv.Itoa(i)+"\n", 5000))
					}(i)
				}
				wg.Wait()
				won := -1
				for i, err := range errs {
					switch KindOf(err) {
					case 0:
						if won >= 0 {
							t.Fatalf("racers %d and %d both won", won, i)
						}
						won = i
					case ErrExists:
					default:
						t.Fatalf("racer %d: %v", i, err)
					}
				}
				if won < 0 {
					t.Fatal("nobody won")
				}
				b, err := os.ReadFile(path)
				if err != nil || string(b) != strings.Repeat("v: "+strconv.Itoa(won)+"\n", 5000) {
					t.Fatalf("file isn't racer %d's whole text: %v", won, err)
				}
				if ents, _ := os.ReadDir(dir); len(ents) != 1 {
					t.Fatalf("%d files left, want 1", len(ents))
				}
				if st, _ := os.Stat(path); runtime.GOOS != "windows" && st.Mode().Perm() != 0o600 {
					t.Fatalf("mode %o, want 600", st.Mode().Perm())
				}
			}
		})
	}
}
