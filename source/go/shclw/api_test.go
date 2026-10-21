// Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
// SPDX-License-Identifier: MIT

package shclw_test

import (
	"errors"
	"math"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"testing"

	"github.com/jim-collier/shclwrap/source/go/shclw"
	"github.com/jim-collier/shclwrap/source/go/shclw3"
)

func TestVerifyPut(t *testing.T) {
	for _, v := range libs {
		d := v.lib.Parse("a: 1\n")
		if err := shclw.VerifyPut(d, "a", shclw.Value{Kind: shclw.KindInt, Int: 1}); err != nil {
			t.Errorf("v%d: matching value: %v", v.n, err)
		}
		err := shclw.VerifyPut(d, "a", shclw.Value{Kind: shclw.KindInt, Int: 2})
		if shclw.KindOf(err) != shclw.ErrNoReadBack {
			t.Errorf("v%d: different value: got %v, want noreadback", v.n, err)
		}
		err = shclw.VerifyPut(d, "nope", shclw.Value{Kind: shclw.KindStrs})
		if shclw.KindOf(err) != shclw.ErrNoReadBack {
			t.Errorf("v%d: missing array: got %v, want noreadback", v.n, err)
		}
	}
}

// A float32 has to go in as its own shortest digits. Widened to float64
// first, 0.9 was written as 0.8999999761581421.
func TestPutFloat32(t *testing.T) {
	texts := map[float32]string{0.9: "f: 0.9\n", 0.1: "f: 0.1\n", -2.5: "f: -2.5\n"}
	for _, v := range libs {
		for _, f := range []float32{0.9, 0.1, -2.5, 1e-10, math.MaxFloat32, 16777216, math.SmallestNonzeroFloat32} {
			d := v.lib.New()
			if err := d.Put("f", f); err != nil {
				t.Errorf("v%d %v: %v", v.n, f, err)
				continue
			}
			want, _ := strconv.ParseFloat(strconv.FormatFloat(float64(f), 'g', -1, 32), 64)
			if g := d.GetFloat("f"); g.Status != shclw.Good || g.Value != want || float32(g.Value) != f {
				t.Errorf("v%d %v: read back %v %v, want %v", v.n, f, g.Status, g.Value, want)
			}
			if text, ok := texts[f]; ok && d.Text() != text {
				t.Errorf("v%d %v: text %q, want %q", v.n, f, d.Text(), text)
			}
		}
		// An exact tie goes to even, but at a float32 power of 2 Go rounds up.
		// Rust and C follow both.
		for bits, want := range map[uint32]float64{0x3b200000: 0.0024414062, 0xbb200000: -0.0024414062, 0x39800000: 0.00024414063} {
			f := math.Float32frombits(bits)
			d := v.lib.New()
			if err := d.Put("f", f); err != nil {
				t.Errorf("v%d %v: %v", v.n, f, err)
				continue
			}
			if g := d.GetFloat("f"); g.Status != shclw.Good || g.Value != want {
				t.Errorf("v%d %v: read back %v %v, want %v", v.n, f, g.Status, g.Value, want)
			}
		}
		for _, f := range []float32{float32(math.NaN()), float32(math.Inf(1))} {
			d := v.lib.New()
			if err := d.Put("f", f); shclw.KindOf(err) != shclw.ErrBadValue || d.Has("f") {
				t.Errorf("v%d %v: got %v, has %v, want badvalue and no edit", v.n, f, err, d.Has("f"))
			}
		}
	}
}

func TestOpen(t *testing.T) {
	dir := t.TempDir()
	write := func(name, text string) string {
		p := filepath.Join(dir, name)
		if err := os.WriteFile(p, []byte(text), 0o644); err != nil {
			t.Fatal(err)
		}
		return p
	}
	newer := write("newer.shcl", "a: 1\n##    Format   9\n")
	binary := write("binary.shcl", "a: \xff\xfe\n")
	missing := filepath.Join(dir, "missing.shcl")
	// A newer file opens read-only, and a raw body's Format line isn't the
	// file's.
	opens := []struct {
		n    int
		open func(string) error
	}{
		{3, func(p string) error {
			d, err := shclw3.Open(p)
			if err == nil && d.ReadOnly() {
				return d.Write(p)
			}
			return err
		}},
	}
	for _, o := range opens {
		for path, want := range map[string]shclw.ErrKind{newer: shclw.ErrTooNew, binary: shclw.ErrIO, missing: shclw.ErrNotFound} {
			if got := shclw.KindOf(o.open(path)); got != want {
				t.Errorf("v%d %s: got %v, want %v", o.n, filepath.Base(path), got, want)
			}
		}
	}
	// A Format line in a raw body is the body's. A real one after it still
	// counts.
	raw := "a: 1\ntmpl:\n\t~~~\n##    Format   9\n\t~~~\n"
	inRaw := write("raw.shcl", raw)
	rawThenReal := write("rawreal.shcl", raw+"##    Format   9\n")
	for _, o := range opens {
		if err := o.open(inRaw); err != nil {
			t.Errorf("v%d raw.shcl: got %v, want no error", o.n, err)
		}
		if got := shclw.KindOf(o.open(rawThenReal)); got != shclw.ErrTooNew {
			t.Errorf("v%d rawreal.shcl: got %v, want toonew", o.n, got)
		}
	}
}

// A 2.x file the old shcl reported errors on is refused, and UpgradeFile
// leaves it as it was, with no backup. A clean one still goes.
func TestUpgradeRefusesOldErrors(t *testing.T) {
	for _, c := range []struct {
		text string
		line int
	}{
		{"a: 1\nb: ```\nraw line\nc: 2\n", 2},
		{"x: [ab]\ny: 1\n", 1},
	} {
		_, err := shclw3.Upgrade(c.text, 2)
		var e *shclw.Error
		if !errors.As(err, &e) || e.Kind != shclw.ErrRefused || len(e.Diags) == 0 || e.Diags[0].Line != c.line {
			t.Errorf("Upgrade %q: got %v, want refused at line %d", c.text, err, c.line)
		}
		dir := t.TempDir()
		path := filepath.Join(dir, "c.shcl")
		if err := os.WriteFile(path, []byte(c.text), 0o644); err != nil {
			t.Fatal(err)
		}
		if _, err := shclw3.UpgradeFile(path, 2); shclw.KindOf(err) != shclw.ErrRefused {
			t.Errorf("UpgradeFile %q: got %v, want refused", c.text, err)
		}
		if b, _ := os.ReadFile(path); string(b) != c.text {
			t.Errorf("UpgradeFile %q: file is now %q", c.text, b)
		}
		if ents, _ := os.ReadDir(dir); len(ents) != 1 {
			t.Errorf("UpgradeFile %q: %d files beside it, want none", c.text, len(ents)-1)
		}
	}
	if u, err := shclw3.Upgrade("tags: a, b\n", 2); err != nil || !u.Changed {
		t.Errorf("clean 2.x file: got %+v, %v, want changed", u, err)
	}
}

// A new document has no lines of its own to keep, so it never rewrites. A
// load at any strictness counts the same as Parse.
func TestRewritesNew(t *testing.T) {
	for _, v := range libs {
		d := v.lib.New()
		if err := d.Put("a.b", 1); err != nil {
			t.Fatalf("v%d: %v", v.n, err)
		}
		if d.Rewrites() {
			t.Errorf("v%d: new document says it rewrites", v.n)
		}
		loose, _ := v.lib.ParseWith("a:   1\n", shclw.Loose)
		if got, want := loose.Rewrites(), v.n < 3; got != want {
			t.Errorf("v%d: loose load of a text that isn't canonical: got %v, want %v", v.n, got, want)
		}
	}
}

// First and Last pick by line, so the fallback reads and an edit take that
// instance alone. An instance a put made has no line and sorts last.
func TestFirstLast(t *testing.T) {
	for _, v := range libs {
		d := v.lib.Parse("p: 1\ntv: a\n\tf: 1\ntv: b\n\tf: 2\ntv: a\n\tf: 3\np: 2\n")
		if got := d.Int(d.Last("p"), 9); got != 2 {
			t.Errorf("v%d: Int of last p: %d, want 2", v.n, got)
		}
		if got := d.Str(d.First("tv.f"), ""); got != "1" {
			t.Errorf("v%d: Str of first tv.f: %q, want 1", v.n, got)
		}
		if got := d.Int(d.Last("tv.f"), 9); got != 3 {
			t.Errorf("v%d: Int of last tv.f: %d, want 3", v.n, got)
		}
		if got := d.Int("p", 9); got != 9 {
			t.Errorf("v%d: plain Int of p: %d, want the fallback", v.n, got)
		}
		if err := d.Put(d.Last("p"), 5); err != nil {
			t.Fatalf("v%d: put on last p: %v", v.n, err)
		}
		if g, l := d.GetInt(d.First("p")), d.Lines("p"); g.Value != 1 || d.Int(d.Last("p"), 9) != 5 || !slices.Equal(l, []int{1, 8}) {
			t.Errorf("v%d: after a put on last p: first %d, last %d, lines %v", v.n, g.Value, d.Int(d.Last("p"), 9), l)
		}
		tv := d.Each("tv")
		if err := d.Put(d.Child(tv[1].Path, "g"), 2); err != nil {
			t.Fatalf("v%d: %v", v.n, err)
		}
		if err := d.Put(d.Child(tv[0].Path, "g"), 1); err != nil {
			t.Fatalf("v%d: %v", v.n, err)
		}
		if got, l := d.Int(d.Last("tv.g"), 9), d.Lines("tv.g"); got != 2 || !slices.Equal(l, []int{0, 0}) {
			t.Errorf("v%d: two put instances: last %d, lines %v, want 2 and [0 0]", v.n, got, l)
		}
		if got := d.Last("nope"); got != "nope" {
			t.Errorf("v%d: Last of a missing path: %q", v.n, got)
		}
	}
}
