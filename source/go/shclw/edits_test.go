// Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
// SPDX-License-Identifier: MIT

package shclw_test

import (
	"reflect"
	"slices"
	"strings"
	"testing"

	"github.com/jim-collier/shclwrap/source/go/shclw"
)

// Every Doc method, sorted. A new one fails TestDocMethodsSorted until it's
// listed here, and a new edit until it's in edits too, so the
// several-instances tests below reach it. Rust's ShclDoc has the same set.
var docMethods = map[string]string{
	"Text": "read", "Canonical": "read", "Write": "read", "Rewrites": "read",
	"Diags": "read", "Errors": "read", "Check": "read", "DropDeclared": "read",
	"Int": "read", "Float": "read", "Bool": "read", "Str": "read",
	"Strs": "read", "Ints": "read", "Floats": "read", "Raw": "read",
	"GetInt": "read", "GetFloat": "read", "GetBool": "read", "GetStr": "read",
	"GetStrs": "read", "GetInts": "read", "GetFloats": "read", "GetRaw": "read",
	"Has": "read", "Count": "read", "Names": "read", "Each": "read",
	"Line": "read", "Lines": "read", "First": "read", "Last": "read", "Child": "read",

	"Put": "edit", "PutSyntax": "edit", "Comment": "edit",
	// Takes every instance, on purpose (design.md, Writes).
	"Remove": "remove",
	// No path; they change what the save writes (design.md, Format and upgrade).
	"InfoBlock": "info", "StampFormat": "info",
	// What Open upgraded, and where the first Write backed it up.
	"UpgradedFrom": "read", "BackupName": "read",
}

var edits = map[string]func(d shclw.Doc, path string) error{
	"Put":       func(d shclw.Doc, p string) error { return d.Put(p, 5) },
	"PutSyntax": func(d shclw.Doc, p string) error { return d.PutSyntax(p, "6") },
	"Comment":   func(d shclw.Doc, p string) error { return d.Comment(p, "c") },
}

func TestDocMethodsSorted(t *testing.T) {
	typ := reflect.TypeOf((*shclw.Doc)(nil)).Elem()
	seen := map[string]bool{}
	for i := 0; i < typ.NumMethod(); i++ {
		name := typ.Method(i).Name
		seen[name] = true
		kind, ok := docMethods[name]
		if !ok {
			t.Errorf("Doc.%s isn't in docMethods", name)
		} else if kind == "edit" && edits[name] == nil {
			t.Errorf("Doc.%s is an edit with no entry in edits", name)
		}
	}
	for name := range docMethods {
		if !seen[name] {
			t.Errorf("docMethods lists %s, which Doc doesn't have", name)
		}
	}
}

// tv and u.v repeat at an inner step, n at the last one. Only one tv has a
// g, and only one v has a w.
const severalDoc = "tv: one\n\tg: 1\ntv: two\nn: 1\nn: 2\nu:\n\tv: 1\n\tv: 2\n\t\tw: 1\n"

// Every edit refuses a path that reaches several instances at any step, as
// 3.0 does, and changes nothing. 1.2 and 2.0 took the first one.
func TestEditsRefuseSeveral(t *testing.T) {
	paths := []string{"n", "n.x", "tv", "tv.g", "tv.h", "tv.g.h", "u.v", "u.v.w", "u.v.w.x"}
	for _, v := range libs {
		for name, edit := range edits {
			for _, p := range paths {
				d := v.lib.Parse(severalDoc)
				was := d.Text()
				err := edit(d, p)
				if shclw.KindOf(err) != shclw.ErrBadPath || d.Text() != was {
					t.Errorf("v%d %s %q: got %v, want badpath and no change; text now %q", v.n, name, p, err, d.Text())
				}
			}
		}
	}
}

// A path from Each, First, or Last names one instance at every step, so an
// edit takes it and changes that one alone.
func TestEditsOnOneInstance(t *testing.T) {
	for _, v := range libs {
		for name, edit := range edits {
			d := v.lib.Parse(severalDoc)
			var paths []string
			for _, n := range d.Each("tv") {
				paths = append(paths, d.Child(n.Path, "h"))
			}
			for _, n := range d.Each("u.v") {
				paths = append(paths, d.Child(n.Path, "w"))
			}
			for _, n := range d.Each("n") {
				paths = append(paths, n.Path)
			}
			paths = append(paths, d.First("tv.g"), d.Last("u.v.w"), d.Last("n"))
			for _, p := range paths {
				d := v.lib.Parse(severalDoc)
				was := d.Text()
				if err := edit(d, p); err != nil {
					t.Errorf("v%d %s %q: %v", v.n, name, p, err)
					continue
				}
				// A put adds or changes a line, and a comment on a new name
				// adds 2. Two instances changed are 2 places, with the lines
				// between them in the hunk.
				gone, added := hunk(was, d.Text())
				if len(gone) > 1 || len(added) > 2 {
					t.Errorf("v%d %s %q: changed more than one place: %q became %q", v.n, name, p, gone, added)
				}
			}
		}
	}
}

// Remove takes every instance, wherever the path repeats, the same on every
// version.
func TestRemoveTakesAll(t *testing.T) {
	for _, v := range libs {
		for _, p := range []string{"n", "tv.g", "u.v", "u.v.w"} {
			d := v.lib.Parse(severalDoc)
			want := d.Count(p)
			if got := d.Remove(p); got != want || d.Has(p) {
				t.Errorf("v%d %q: removed %d of %d, still has it %v", v.n, p, got, want, d.Has(p))
			}
		}
	}
}

// Each on a wildcard lists the nodes it reaches, each read alone and with
// its line. A slot that reaches no node is left out, as 3.0 leaves it.
func TestEachWildcard(t *testing.T) {
	doc := "a:\n\tx:\n\t\tb: 1\n\tz:\n\t\tb: 2\n\ty:\n\t\tc: 3\n\tx: again\n\t\tb: 4\n"
	for _, v := range libs {
		d := v.lib.Parse(doc)
		var lines []int
		var vals []int64
		for _, n := range d.Each("a.*.b") {
			lines = append(lines, n.Line)
			vals = append(vals, d.GetInt(n.Path).Value)
		}
		if !slices.Equal(lines, []int{3, 5, 9}) || !slices.Equal(vals, []int64{1, 2, 4}) {
			t.Errorf("v%d: lines %v, values %v", v.n, lines, vals)
		}
		if got := d.Int(d.Last("a.*.b"), 0); got != 4 {
			t.Errorf("v%d: last of a.*.b: %d, want 4", v.n, got)
		}
	}
}

// hunk is the lines of a and b between their common start and end.
func hunk(a, b string) (gone, added []string) {
	x, y := strings.Split(a, "\n"), strings.Split(b, "\n")
	for len(x) > 0 && len(y) > 0 && x[0] == y[0] {
		x, y = x[1:], y[1:]
	}
	for len(x) > 0 && len(y) > 0 && x[len(x)-1] == y[len(y)-1] {
		x, y = x[:len(x)-1], y[:len(y)-1]
	}
	return x, y
}
