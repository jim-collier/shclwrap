// Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
// SPDX-License-Identifier: MIT

// Command shclw-drive runs an ops script against a document with one shcl
// version, for the tests that compare wrapper languages.
//
//	shclw-drive VERSION DOCFILE [DIR] < SCRIPT
//
// VERSION is 3. DIR is where the script's check and write ops find
// and put files.
package main

import (
	"errors"
	"fmt"
	"io"
	"os"
	"unicode/utf8"

	"github.com/jim-collier/shclwrap/source/go/shclw"
	"github.com/jim-collier/shclwrap/source/go/shclw3"
)

var libs = map[string]shclw.Lib{"3": shclw3.Lib}

func main() {
	if len(os.Args) < 3 || len(os.Args) > 4 {
		fmt.Fprintln(os.Stderr, "usage: shclw-drive VERSION DOCFILE [DIR] < SCRIPT")
		os.Exit(2)
	}
	lib, ok := libs[os.Args[1]]
	if !ok {
		fmt.Fprintln(os.Stderr, "shclw-drive: no such version:", os.Args[1])
		os.Exit(2)
	}
	doc, err := shclw.ReadText(os.Args[2])
	if err != nil {
		fmt.Fprintln(os.Stderr, "shclw-drive:", err)
		os.Exit(2)
	}
	script, err := io.ReadAll(os.Stdin)
	if err == nil && !utf8.Valid(script) {
		err = errors.New("script is not valid UTF-8")
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "shclw-drive:", err)
		os.Exit(2)
	}
	var opt shclw.RunOptions
	if len(os.Args) == 4 {
		opt.Dir = os.Args[3]
	}
	fmt.Print(shclw.Run(lib, doc, string(script), opt))
}
