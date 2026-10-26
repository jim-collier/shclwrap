// Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
// SPDX-License-Identifier: MIT

package shclw_test

import (
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	s3 "github.com/yottacore/shcl/source/go/v2"

	"github.com/jim-collier/shclwrap/source/go/shclw"
)

// Every info block shcl's GenBanner has been, in order, and 2.x's. The
// betas linked tags that were never cut.
var oldBlocks = []string{
	"#\n# This config file format is SHCL.\n# \"Simple Hierarchical Config Language\"\n#    Home     https://github.com/jim-collier/shcl\n#    Syntax   https://github.com/jim-collier/shcl/blob/main/project/spec.md\n#    Legal    SHCL is Copyright © 2026 Jim Collier. License: MIT. No warranty.\n#\n",
	"##\n## This config file format is SHCL.\n## \"Simple Hierarchical Config Language\"\n##    Home     https://github.com/jim-collier/shcl\n##    Syntax   https://github.com/jim-collier/shcl/blob/main/project/spec.md\n##    Legal    SHCL is Copyright © 2026 Jim Collier. License: MIT. No warranty.\n##\n",
	"##\n## This config file format is SHCL.\n## \"Simple Hierarchical Config Language\"\n##    Home     https://github.com/jim-collier/shcl\n##    Syntax   https://github.com/jim-collier/shcl/blob/main/project/spec.md\n##    Legal    SHCL is Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]. License: MIT. No warranty.\n##\n",
	"##\n## This config file format is SHCL.\n## \"Simple Hierarchical Config Language\"\n##    Format   %d\n##    Home     https://github.com/jim-collier/shcl\n##    Syntax   https://github.com/jim-collier/shcl/blob/main/project/spec.md\n##    Legal    SHCL is Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]. License: MIT. No warranty.\n##\n",
	"##\n## This config file format is SHCL.\n## \"Simple Hierarchical Config Language\"\n##    Format   %d\n##    Home     https://github.com/jim-collier/shcl\n##    Syntax   https://github.com/jim-collier/shcl/blob/v3.0.0/project/spec.md\n##    Legal    SHCL is Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]. License: MIT. No warranty.\n##\n",
	"##\n## This config file format is SHCL.\n## \"Simple Hierarchical Config Language\"\n##    Format   %d\n##    Home     https://github.com/yottacore/shcl\n##    Syntax   https://github.com/yottacore/shcl/blob/v3.0.0/project/spec.md\n##    Legal    SHCL is Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]. License: MIT. No warranty.\n##\n",
	s3.GenBanner,
}

// Off since 3.0 is the floor: InfoText was the 1.2 and 2.0 block, and is gone.
// func TestInfoTextIsGenBanner(t *testing.T) {
// 	if got := shclw.InfoText(s3.FormatMajor, "v3.0.0-beta1"); got != s3.GenBanner {
// 		t.Errorf("InfoText(3) =\n%s\nshcl's GenBanner is\n%s", got, s3.GenBanner)
// 	}
// }

// infoOn is text with InfoBlock(true), and the block it ends with taken
// off, so the versions compare.
func infoOn(t *testing.T, n int, lib shclw.Lib, text string) string {
	t.Helper()
	d := lib.Parse(text)
	if err := d.InfoBlock(true); err != nil {
		t.Fatalf("v%d: InfoBlock on %q: %v", n, text, err)
	}
	got := d.Text()
	block := s3.GenBanner
	if !strings.HasSuffix(got, block) || strings.Count(got, "This config file format is SHCL.") != 1 {
		t.Fatalf("v%d: text after InfoBlock on %q is\n%s", n, text, got)
	}
	return strings.TrimSuffix(got, block)
}

// Each old spelling, above the first field, between two, and at the end,
// becomes one block at the end, and every version leaves the same text.
// Test ID: de036
func TestInfoOldSpellings(t *testing.T) {
	for i, old := range oldBlocks {
		for _, at := range []string{"%sa: 1\nb: 2\n", "a: 1\n%sb: 2\n", "a: 1\nb: 2\n\n%s"} {
			var first string
			for _, v := range libs {
				block := strings.ReplaceAll(old, "%d", strconv.Itoa(v.n))
				got := infoOn(t, v.n, v.lib, strings.Replace(at, "%s", block, 1))
				if first == "" {
					first = got
				} else if got != first {
					t.Errorf("spelling %d at %q: v%d left %q, an older version %q", i, at, v.n, got, first)
				}
				if got != "a: 1\nb: 2\n\n" {
					t.Errorf("spelling %d at %q: v%d left %q", i, at, v.n, got)
				}
			}
		}
	}
}

// Where shcl 3.0's own SetBanner finds the old block, the wrapper writes
// what it writes.
// Test ID: de035
func TestInfoMatchesSetBanner(t *testing.T) {
	for _, text := range []string{
		"a: 1\nb: 2\n",
		"",
		"a: 1\n" + s3.GenBanner + "b: 2\n",
		"a: 1\n\n" + s3.GenBanner + "\nb: 2\n",
		"# top\na: 1\nb:\n\tc: 2\n\n" + s3.GenBanner,
		"a: 1\nb:\n\tc: 2\n" + s3.FormatLine + "\n" + s3.MigratedLine + "\n",
		"a: 1\n## mine\n" + s3.GenBanner + "## also mine\nb: 2\n",
	} {
		sd, err := s3.ParseKeepLines(text, s3.Standard)
		if err != nil {
			t.Fatal(err)
		}
		sd.SetBanner(true)
		want, kept := sd.ToTextKeepLines()
		if !kept {
			want = sd.ToCanonical()
		}
		for _, on := range []bool{true, false} {
			d := libs[0].lib.Parse(text)
			if err := d.InfoBlock(on); err != nil {
				t.Fatalf("%q: %v", text, err)
			}
			if !on {
				sd, _ = s3.ParseKeepLines(text, s3.Standard)
				sd.SetBanner(false)
				want, _ = sd.ToTextKeepLines()
			}
			if got := d.Text(); got != want {
				t.Errorf("InfoBlock(%v) on %q:\n%s\nSetBanner(%v):\n%s", on, text, got, on, want)
			}
		}
	}
}

// An edit after InfoBlock that names an older format leaves Text without
// the block, and Write refuses, writing nothing.
// Test ID: de037
func TestInfoWriteRefused(t *testing.T) {
	dir := t.TempDir()
	for _, v := range libs {
		d := v.lib.Parse("a: 1\n")
		if err := d.InfoBlock(true); err != nil {
			t.Fatalf("v%d: %v", v.n, err)
		}
		if err := d.Comment("a", "##    Format   9"); err != nil {
			t.Fatalf("v%d: %v", v.n, err)
		}
		if strings.Contains(d.Text(), "SHCL") {
			t.Errorf("v%d: text after a newer Format line is\n%s", v.n, d.Text())
		}
		path := filepath.Join(dir, "v"+strconv.Itoa(v.n)+".shcl")
		if err := d.Write(path); shclw.KindOf(err) != shclw.ErrTooNew {
			t.Errorf("v%d: Write gave %v, want toonew", v.n, err)
		}
		if _, err := os.Stat(path); !os.IsNotExist(err) {
			t.Errorf("v%d: Write left a file: %v", v.n, err)
		}
	}
}
