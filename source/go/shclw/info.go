// Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
// SPDX-License-Identifier: MIT

package shclw

import (
	"fmt"
	"math"
	"strconv"
	"strings"
)

// Info is what a save does with the info block.
type Info int

const (
	InfoAsIs Info = iota
	InfoOn
	InfoOff
)

// InfoEdit is what an adapter hands FinishInfo. Values sums up the values
// and errors text loads to with the bound shcl. A run of lines whose removal
// leaves that the same is comments, which is how a block's text inside a raw
// body is told from a real block.
type InfoEdit struct {
	Block  string
	Major  int
	Values func(text string) string
}

const (
	infoTitle    = " This config file format is SHCL."
	infoName     = ` "Simple Hierarchical Config Language"`
	formatWord   = "##    Format"
	migratedLine = "##    Migrated from SHCL 2.x."
	schemaHead   = "##    Schema   "
)

// FinishInfo is text as a save writes it after InfoBlock and StampFormat
// (design.md, Format and upgrade).
func FinishInfo(text string, info Info, stamp bool, e InfoEdit) (string, error) {
	if info == InfoAsIs && !stamp {
		return text, nil
	}
	bom := ""
	if strings.HasPrefix(text, "\ufeff") {
		bom, text = "\ufeff", text[len("\ufeff"):]
	}
	f := splitInfoLines(text)
	base := e.Values(text)
	comments := func(drop []bool) bool { return e.Values(f.joinWithout(drop)) == base }

	named := false
	for _, n := range formatsNamed(f, e, base) {
		switch {
		case n > e.Major:
			return "", &Error{Kind: ErrTooNew, Msg: fmt.Sprintf("the file names format %d", n)}
		case n < e.Major:
			return "", &Error{Kind: ErrRefused, Msg: fmt.Sprintf("the file names format %d; upgrade it first", n)}
		}
		named = true
	}

	if info != InfoAsIs {
		named = false
		drop := make([]bool, len(f.lines))
		for _, r := range infoRuns(f) {
			// A lone stamp the save would add back stays where it is.
			if n, lone := formatNumber(f.trimmed(r[0]), e.Major); lone && stamp && info == InfoOff && n == e.Major {
				named = true
				continue
			}
			run := span(len(f.lines), r[0], r[1])
			for i := r[0]; i < r[1]; i++ {
				// shcl keeps a Schema line that sits in the block.
				run[i] = !strings.HasPrefix(f.trimmed(i), schemaHead)
			}
			if comments(run) {
				for i := range run {
					drop[i] = drop[i] || run[i]
				}
			}
		}
		f.remove(drop)
	}
	before := f.join()
	if info == InfoOn {
		f.trimBlankTail()
		if len(f.lines) > 0 {
			f.add("")
		}
		for _, l := range strings.Split(strings.TrimSuffix(e.Block, "\n"), "\n") {
			f.add(l)
		}
		named = true
	}
	if stamp && !named {
		f.add(formatWord + "   " + strconv.Itoa(e.Major))
	}
	out := f.join()
	if out != before && e.Values(out) != e.Values(before) {
		return "", &Error{Kind: ErrRefused, Msg: "the file ends in a raw block that never closes"}
	}
	return bom + out, nil
}

// formatsNamed is the number on each Format line that counts, in order. A
// line counts when taking it out leaves the values at base, so one in a raw
// body doesn't.
func formatsNamed(f *infoLines, e InfoEdit, base string) []int {
	var named []int
	for i := range f.lines {
		n, ok := formatNumber(f.trimmed(i), e.Major)
		if !ok || n == 0 || e.Values(f.joinWithout(span(len(f.lines), i, i+1))) != base {
			continue
		}
		named = append(named, n)
	}
	return named
}

// formatNumber reads a Format line as shcl 3.0 does: the head, then digits
// only. ok is true for any line with the head, which comes off as a stamp,
// and the number is 0 when it doesn't read. Digits too big read as major,
// as shcl reads them.
func formatNumber(trimmed string, major int) (int, bool) {
	if trimmed == formatWord {
		return 0, true
	}
	digits, ok := strings.CutPrefix(trimmed, formatWord+"   ")
	if !ok {
		return 0, false
	}
	if digits == "" || strings.Trim(digits, "0123456789") != "" {
		return 0, true
	}
	n, err := strconv.Atoi(digits)
	if err != nil || uint64(n) > math.MaxUint32 {
		return major, true
	}
	return n, true
}

// infoRuns finds each info block and lone Format line, as [start, end). A
// block runs from the lone mark above its title line to the next lone mark,
// or, with none, through the lines written the block's way. That's shcl
// 3.0's rule for its own setter, with 2.x's `#` block found the same way.
// A Format line outside a block comes with the migrated note under it.
func infoRuns(f *infoLines) [][2]int {
	var runs [][2]int
	n := len(f.lines)
	for i := 0; i < n; {
		t := f.trimmed(i)
		mark := ""
		switch t {
		case "##" + infoTitle:
			mark = "##"
		case "#" + infoTitle:
			mark = "#"
		}
		if mark == "" {
			end := i + 1
			if _, ok := formatNumber(t, 0); ok {
				if end < n && f.trimmed(end) == migratedLine {
					end++
				}
				runs = append(runs, [2]int{i, end})
			}
			i = end
			continue
		}
		start, end := i, i+1
		if i > 0 && f.trimmed(i-1) == mark && (len(runs) == 0 || runs[len(runs)-1][1] < i) {
			start = i - 1
		}
		closed := false
		for k := end; k < n && strings.HasPrefix(f.trimmed(k), mark); k++ {
			if f.trimmed(k) == mark {
				end, closed = k+1, true
				break
			}
		}
		for !closed && end < n && (strings.HasPrefix(f.trimmed(end), mark+"    ") || f.trimmed(end) == mark+infoName) {
			end++
		}
		runs = append(runs, [2]int{start, end})
		i = end
	}
	return runs
}

func span(n, start, end int) []bool {
	b := make([]bool, n)
	for i := start; i < end; i++ {
		b[i] = true
	}
	return b
}

// infoLines is text as lines, each with its own line end, so a CRLF file
// keeps them. An added line ends the way most lines in the file do, LF on a
// tie, as shcl's stamp does.
type infoLines struct {
	lines []string
	ends  []string
	eol   string
}

func splitInfoLines(text string) *infoLines {
	f := &infoLines{eol: "\n"}
	crlf := 0
	for text != "" {
		line, end := text, ""
		if i := strings.IndexByte(text, '\n'); i >= 0 {
			line, end = text[:i], "\n"
			if strings.HasSuffix(line, "\r") {
				line, end = line[:len(line)-1], "\r\n"
				crlf++
			}
		}
		f.lines = append(f.lines, line)
		f.ends = append(f.ends, end)
		text = text[len(line)+len(end):]
	}
	if crlf*2 > len(f.lines) {
		f.eol = "\r\n"
	}
	return f
}

// trimmed is line i without its indent or trailing blanks. Only ASCII ones,
// so Go and Rust agree.
func (f *infoLines) trimmed(i int) string { return strings.Trim(f.lines[i], " \t\r") }

func (f *infoLines) join() string { return f.joinWithout(nil) }

func (f *infoLines) joinWithout(drop []bool) string {
	var b strings.Builder
	for i, l := range f.lines {
		if drop == nil || !drop[i] {
			b.WriteString(l)
			b.WriteString(f.ends[i])
		}
	}
	return b.String()
}

func (f *infoLines) add(line string) {
	if n := len(f.ends); n > 0 && f.ends[n-1] == "" {
		f.ends[n-1] = f.eol
	}
	f.lines = append(f.lines, line)
	f.ends = append(f.ends, f.eol)
}

// remove takes out the dropped lines. Where a run went, the blank lines
// around it become one, or none at the top or the end of the file, so what
// was set apart stays apart.
func (f *infoLines) remove(drop []bool) {
	var lines, ends []string
	for i := 0; i < len(f.lines); {
		if !drop[i] && !blank(f.lines[i]) {
			lines, ends = append(lines, f.lines[i]), append(ends, f.ends[i])
			i++
			continue
		}
		// A stretch of blank and dropped lines.
		k, gone, firstBlank := i, false, -1
		for ; k < len(f.lines) && (drop[k] || blank(f.lines[k])); k++ {
			gone = gone || drop[k]
			if !drop[k] && firstBlank < 0 {
				firstBlank = k
			}
		}
		switch {
		case !gone:
			lines, ends = append(lines, f.lines[i:k]...), append(ends, f.ends[i:k]...)
		case firstBlank >= 0 && len(lines) > 0 && k < len(f.lines):
			lines, ends = append(lines, f.lines[firstBlank]), append(ends, f.ends[firstBlank])
		}
		i = k
	}
	f.lines, f.ends = lines, ends
}

func (f *infoLines) trimBlankTail() {
	for len(f.lines) > 0 && blank(f.lines[len(f.lines)-1]) {
		f.lines, f.ends = f.lines[:len(f.lines)-1], f.ends[:len(f.ends)-1]
	}
}

func blank(l string) bool { return strings.Trim(l, " \t\r") == "" }
