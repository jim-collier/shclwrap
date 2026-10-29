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
	"syscall"
	"testing"
)

// A link error that says the filesystem has no hard links falls back to the
// exclusive create, and any other link error is ErrIO. Errno 1 is what FAT
// gives: EPERM on Linux, ERROR_INVALID_FUNCTION on Windows.
// Test ID: dejdf
func TestCreateNoLinks(t *testing.T) {
	defer func(old func(string, string) error) { linkFile = old }(linkFile)
	for _, c := range []struct {
		err  syscall.Errno
		want ErrKind
	}{{syscall.Errno(1), 0}, {syscall.ENOTSUP, 0}, {syscall.EIO, ErrIO}} {
		linkFile = func(old, new string) error { return &os.LinkError{Op: "link", Old: old, New: new, Err: c.err} }
		dir := t.TempDir()
		path := filepath.Join(dir, "a.shcl")
		if err := CreateNew(path, "a: 1\n", 0); KindOf(err) != c.want {
			t.Fatalf("link gives %v: %v, want kind %v", c.err, err, c.want)
		}
		want := []string{"a.shcl"}
		if c.want != 0 {
			want = nil
		} else if b, err := os.ReadFile(path); err != nil || string(b) != "a: 1\n" {
			t.Fatalf("link gives %v: file %q, %v", c.err, b, err)
		}
		ents, _ := os.ReadDir(dir)
		if len(ents) != len(want) {
			t.Fatalf("link gives %v: %d files left, want %d", c.err, len(ents), len(want))
		}
	}
}

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
