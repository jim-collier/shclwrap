// Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
// SPDX-License-Identifier: MIT

package shclw_test

import (
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"testing"

	"github.com/jim-collier/shclwrap/source/go/shclw"
	"github.com/jim-collier/shclwrap/source/go/shclw3"
)

var libs = []struct {
	n   int
	lib shclw.Lib
}{{3, shclw3.Lib}}

const casesDir = "../../../tests/cases"

// TestCases runs every shared case against every shcl version and compares
// with the case's want.vN.txt, the same files the Rust tests read. Set
// SHCLW_UPDATE=1 to write the want files instead.
func TestCases(t *testing.T) {
	dirs, err := os.ReadDir(casesDir)
	if err != nil {
		t.Fatal(err)
	}
	update := os.Getenv("SHCLW_UPDATE") == "1"
	ran := 0
	for _, c := range dirs {
		if !c.IsDir() {
			continue
		}
		for _, v := range libs {
			name := c.Name() + "/v" + strconv.Itoa(v.n)
			t.Run(name, func(t *testing.T) {
				dir := t.TempDir()
				if !materialize(t, filepath.Join(casesDir, c.Name()), dir, v.n) {
					t.Skip("no doc for this version")
				}
				ran++
				doc := mustRead(t, filepath.Join(dir, "doc.shcl"))
				ops := mustRead(t, filepath.Join(dir, "ops.txt"))
				got := shclw.Run(v.lib, doc, ops, shclw.RunOptions{Dir: dir})
				wantPath := filepath.Join(casesDir, c.Name(), "want.v"+strconv.Itoa(v.n)+".txt")
				if update {
					if err := os.WriteFile(wantPath, []byte(got), 0o644); err != nil {
						t.Fatal(err)
					}
					return
				}
				want, err := os.ReadFile(wantPath)
				if err != nil {
					t.Fatalf("no want file; run with SHCLW_UPDATE=1 and review it: %v", err)
				}
				if got != string(want) {
					t.Errorf("output differs from %s:\n--- got\n%s", wantPath, got)
				}
			})
		}
	}
	if ran == 0 {
		t.Fatal("no cases ran")
	}
}

var verFile = regexp.MustCompile(`^(.+)\.v(\d+)(\.[^.]+)$`)

// materialize copies a case into dir as version n sees it. A file named
// x.vK.ext stands in for x.ext from version K up, until a higher K takes
// over.
func materialize(t *testing.T, from, dir string, n int) bool {
	t.Helper()
	ents, err := os.ReadDir(from)
	if err != nil {
		t.Fatal(err)
	}
	best := map[string]int{}
	src := map[string]string{}
	names := []string{}
	for _, e := range ents {
		base, k := e.Name(), 0
		if m := verFile.FindStringSubmatch(e.Name()); m != nil {
			base = m[1] + m[3]
			k, _ = strconv.Atoi(m[2])
		}
		if regexp.MustCompile(`^want\.`).MatchString(base) || k > n {
			continue
		}
		if prev, ok := best[base]; ok && prev >= k {
			continue
		}
		if _, ok := best[base]; !ok {
			names = append(names, base)
		}
		best[base] = k
		src[base] = e.Name()
	}
	sort.Strings(names)
	for _, base := range names {
		b := mustRead(t, filepath.Join(from, src[base]))
		if err := os.WriteFile(filepath.Join(dir, base), []byte(b), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	_, ok := src["doc.shcl"]
	return ok
}

func mustRead(t *testing.T, path string) string {
	t.Helper()
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}
