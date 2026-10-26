// Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
// SPDX-License-Identifier: MIT

package shclw_test

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/jim-collier/shclwrap/source/go/shclw"
)

// FuzzRun feeds any document and any script to every version. Nothing may
// panic. File ops are off, since RunOptions.Dir is empty.
// Test ID: ddzw4
func FuzzRun(f *testing.F) {
	docs, _ := filepath.Glob(filepath.Join(casesDir, "*", "doc*.shcl"))
	for _, p := range docs {
		doc, err := os.ReadFile(p)
		if err != nil {
			f.Fatal(err)
		}
		ops, err := os.ReadFile(filepath.Join(filepath.Dir(p), "ops.txt"))
		if err != nil {
			f.Fatal(err)
		}
		f.Add(string(doc), string(ops))
	}
	if len(docs) == 0 {
		f.Fatal("no seed documents")
	}
	f.Fuzz(func(t *testing.T, doc, script string) {
		for _, v := range libs {
			shclw.Run(v.lib, doc, script, shclw.RunOptions{})
		}
	})
}
