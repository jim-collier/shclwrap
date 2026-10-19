// Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
// SPDX-License-Identifier: MIT

package shclw_test

import (
	"errors"
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/jim-collier/shclwrap/source/go/shclw"
	"github.com/jim-collier/shclwrap/source/go/shclw3"
	shcl3 "github.com/yottacore/shcl/source/go/v2"
)

const (
	clean2x  = "tags: a, b\n"
	clean2to = "tags: [a, b]\n"
)

func writeFile(t *testing.T, path, text string, mode os.FileMode) {
	t.Helper()
	if err := os.WriteFile(path, []byte(text), mode); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(path, mode); err != nil {
		t.Fatal(err)
	}
}

func readFile(t *testing.T, path string) string {
	t.Helper()
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}

func names(t *testing.T, dir string) []string {
	t.Helper()
	ents, err := os.ReadDir(dir)
	if err != nil {
		t.Fatal(err)
	}
	var out []string
	for _, e := range ents {
		out = append(out, e.Name())
	}
	return out
}

// hasMode is true on Windows, which keeps no Unix modes.
func hasMode(t *testing.T, path string, mode os.FileMode) bool {
	t.Helper()
	st, err := os.Stat(path)
	if err != nil {
		t.Fatal(err)
	}
	return runtime.GOOS == "windows" || st.Mode().Perm() == mode
}

// readOnlyDir is a folder nothing can be added to or renamed in, or a skip
// where the filesystem or the user ignores the mode.
func readOnlyDir(t *testing.T, files map[string]string) string {
	t.Helper()
	dir := t.TempDir()
	for name, text := range files {
		writeFile(t, filepath.Join(dir, name), text, 0o644)
	}
	if err := os.Chmod(dir, 0o555); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { os.Chmod(dir, 0o755) })
	probe := filepath.Join(dir, "probe")
	if err := os.WriteFile(probe, nil, 0o644); err == nil {
		os.Remove(probe)
		t.Skip("a read-only folder still takes new files here")
	}
	return dir
}

func TestReplaceRead(t *testing.T) {
	const was, now = "a: 1\n", "a: 2\n"
	boom := errors.New("boom")
	setup := func(t *testing.T) (string, string, func() (string, error)) {
		dir := t.TempDir()
		path := filepath.Join(dir, "c.shcl")
		writeFile(t, path, was, 0o640)
		bak := filepath.Join(dir, "c.bak")
		return dir, path, func() (string, error) { writeFile(t, bak, was, 0o640); return bak, nil }
	}
	plain := func(path, data string) error { return os.WriteFile(path, []byte(data), 0o640) }

	t.Run("ok", func(t *testing.T) {
		_, path, backup := setup(t)
		name, err := shclw.ReplaceRead(path, was, now, backup, plain)
		if err != nil || readFile(t, path) != now || readFile(t, name) != was {
			t.Errorf("got %q, %v", name, err)
		}
	})
	t.Run("changed before", func(t *testing.T) {
		dir, path, _ := setup(t)
		called := false
		_, err := shclw.ReplaceRead(path, "a: 0\n", now, func() (string, error) { called = true; return "", nil }, plain)
		if shclw.KindOf(err) != shclw.ErrIO || called || readFile(t, path) != was || len(names(t, dir)) != 1 {
			t.Errorf("got %v, backup called %v, file %q, %v", err, called, readFile(t, path), names(t, dir))
		}
	})
	t.Run("changed during backup", func(t *testing.T) {
		dir, path, backup := setup(t)
		edit := func() (string, error) {
			name, err := backup()
			writeFile(t, path, "a: 3\n", 0o640)
			return name, err
		}
		_, err := shclw.ReplaceRead(path, was, now, edit, plain)
		if shclw.KindOf(err) != shclw.ErrIO || readFile(t, path) != "a: 3\n" || len(names(t, dir)) != 1 {
			t.Errorf("got %v, file %q, %v", err, readFile(t, path), names(t, dir))
		}
	})
	t.Run("backup fails", func(t *testing.T) {
		dir, path, _ := setup(t)
		taken := &shclw.Error{Kind: shclw.ErrBackupTaken}
		_, err := shclw.ReplaceRead(path, was, now, func() (string, error) { return "", taken }, plain)
		if err != taken || readFile(t, path) != was || len(names(t, dir)) != 1 {
			t.Errorf("got %v, file %q", err, readFile(t, path))
		}
	})
	t.Run("write fails", func(t *testing.T) {
		dir, path, backup := setup(t)
		_, err := shclw.ReplaceRead(path, was, now, backup, func(string, string) error { return boom })
		if shclw.KindOf(err) != shclw.ErrIO || !errors.Is(err, boom) || readFile(t, path) != was || len(names(t, dir)) != 1 {
			t.Errorf("got %v, file %q, %v", err, readFile(t, path), names(t, dir))
		}
	})
	t.Run("write fails and takes the file", func(t *testing.T) {
		dir, path, backup := setup(t)
		gone := func(path, _ string) error { os.Remove(path); return boom }
		_, err := shclw.ReplaceRead(path, was, now, backup, gone)
		if shclw.KindOf(err) != shclw.ErrIO || readFile(t, path) != was || !hasMode(t, path, 0o640) || len(names(t, dir)) != 1 {
			t.Errorf("got %v, file %q, %v", err, readFile(t, path), names(t, dir))
		}
	})
	t.Run("write fails part way", func(t *testing.T) {
		_, path, backup := setup(t)
		half := func(path, _ string) error { os.WriteFile(path, []byte("a:"), 0o640); return boom }
		name, err := shclw.ReplaceRead(path, was, now, backup, half)
		bak := filepath.Join(filepath.Dir(path), "c.bak")
		if shclw.KindOf(err) != shclw.ErrIO || name != "" || !strings.Contains(err.Error(), bak) || readFile(t, bak) != was {
			t.Errorf("got %q, %v", name, err)
		}
	})
	t.Run("no backup", func(t *testing.T) {
		dir, path, _ := setup(t)
		if _, err := shclw.ReplaceRead(path, was, now, nil, plain); err != nil || readFile(t, path) != now || len(names(t, dir)) != 1 {
			t.Errorf("got %v, %v", err, names(t, dir))
		}
	})
}

// An edit made while UpgradeFile runs must not get past the 2.0 check. The
// file flips between a clean 2.x text adn one 2.0 has errors on, so a backup
// holding the bad text means that text was converted unchecked.
func TestUpgradeFileEditedMeanwhile(t *testing.T) {
	const bad = "x: [ab]\ny: 1\n"
	root := t.TempDir()
	deadline := time.Now().Add(2 * time.Second)
	upgraded := 0
	for i := 0; i < 200 && time.Now().Before(deadline); i++ {
		dir := filepath.Join(root, strconv.Itoa(i))
		if err := os.Mkdir(dir, 0o755); err != nil {
			t.Fatal(err)
		}
		path := filepath.Join(dir, "c.shcl")
		writeFile(t, path, clean2x, 0o644)
		stop, done := make(chan struct{}), make(chan struct{})
		go func() {
			defer close(done)
			tmp := filepath.Join(dir, "flip")
			for n := 0; ; n++ {
				select {
				case <-stop:
					return
				default:
				}
				text := clean2x
				if n%2 == 0 {
					text = bad
				}
				if os.WriteFile(tmp, []byte(text), 0o644) != nil || os.Rename(tmp, path) != nil {
					return
				}
			}
		}()
		u, err := shclw3.UpgradeFile(path, 2)
		close(stop)
		<-done
		if err != nil || u.Backup == "" {
			continue
		}
		upgraded++
		if b := readFile(t, u.Backup); b != clean2x {
			t.Fatalf("round %d: backup holds %q, so that text was converted", i, b)
		}
	}
	if upgraded == 0 {
		t.Error("no round got as far as an upgrade")
	}
}

func TestUpgradeRealFile(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "c.shcl")
	writeFile(t, path, clean2x, 0o600)
	u, err := shclw3.UpgradeFile(path, 2)
	if err != nil || !u.Changed || u.From != 2 || !strings.HasPrefix(u.Text, clean2to) || shclw3.FormatOf(u.Text) != 3 {
		t.Fatalf("got %+v, %v", u, err)
	}
	if got := readFile(t, path); got != u.Text {
		t.Errorf("file holds %q, want %q", got, u.Text)
	}
	if filepath.Dir(u.Backup) != dir || readFile(t, u.Backup) != clean2x {
		t.Errorf("backup %q holds %q", u.Backup, readFile(t, u.Backup))
	}
	for _, p := range []string{path, u.Backup} {
		if !hasMode(t, p, 0o600) {
			t.Errorf("%s: want mode 0600", filepath.Base(p))
		}
	}
	if len(names(t, dir)) != 2 {
		t.Errorf("files: %v", names(t, dir))
	}
	u, err = shclw3.UpgradeFile(path, 0)
	if err != nil || u.Changed || u.Backup != "" || len(names(t, dir)) != 2 {
		t.Errorf("second run: %+v, %v, %v", u, err, names(t, dir))
	}
	newer := filepath.Join(t.TempDir(), "n.shcl")
	writeFile(t, newer, "a: 1\n##    Format   9\n", 0o644)
	if _, err := shclw3.UpgradeFile(newer, 0); shclw.KindOf(err) != shclw.ErrTooNew || readFile(t, newer) != "a: 1\n##    Format   9\n" {
		t.Errorf("format 9 file: got %v", err)
	}
}

// Something already at the backup's name is never written over, and the
// file stays as it was.
func TestUpgradeBackupTaken(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "c.shcl")
	writeFile(t, path, clean2x, 0o644)
	// The name has the time in it to the second, so a tick between taking
	// it and the upgrade makes another. Go again when that happens.
	for try := 0; try < 3; try++ {
		name := shcl3.BackupFileName(path, 2)
		writeFile(t, name, "mine\n", 0o644)
		_, err := shclw3.UpgradeFile(path, 2)
		if shcl3.BackupFileName(path, 2) != name {
			os.Remove(name)
			for _, n := range names(t, dir) {
				if n != "c.shcl" {
					os.Remove(filepath.Join(dir, n))
				}
			}
			writeFile(t, path, clean2x, 0o644)
			continue
		}
		if shclw.KindOf(err) != shclw.ErrBackupTaken {
			t.Errorf("got %v, want backuptaken", err)
		}
		if readFile(t, path) != clean2x || readFile(t, name) != "mine\n" || len(names(t, dir)) != 2 {
			t.Errorf("file %q, backup %q, %v", readFile(t, path), readFile(t, name), names(t, dir))
		}
		return
	}
	t.Fatal("the clock ticked on every try")
}

// A save the folder won't take is ErrIO, and the file there stays as it
// was, on every version. So does an upgrade, which then leaves no backup.
func TestSaveRefusedByFolder(t *testing.T) {
	const text = "a: 1\n"
	dir := readOnlyDir(t, map[string]string{"c.shcl": text, "u.shcl": clean2x})
	for _, v := range libs {
		d := v.lib.Parse("a: 2\n")
		for _, name := range []string{"c.shcl", "new.shcl"} {
			if err := d.Write(filepath.Join(dir, name)); shclw.KindOf(err) != shclw.ErrIO {
				t.Errorf("v%d %s: got %v, want io", v.n, name, err)
			}
		}
	}
	if _, err := shclw3.UpgradeFile(filepath.Join(dir, "u.shcl"), 2); shclw.KindOf(err) != shclw.ErrIO {
		t.Errorf("upgrade: got %v, want io", err)
	}
	if readFile(t, filepath.Join(dir, "c.shcl")) != text || readFile(t, filepath.Join(dir, "u.shcl")) != clean2x || len(names(t, dir)) != 2 {
		t.Errorf("folder now holds %v", names(t, dir))
	}
}

// A save that would drop lines the load couldn't use is refused, and the
// file it was read from stays as it was.
func TestSaveRefusedOverFile(t *testing.T) {
	for _, c := range []struct {
		n    int
		text string
		open func(string) (shclw.Doc, error)
	}{
		{3, "a: 1\na(5): 2\n", func(p string) (shclw.Doc, error) { return shclw3.Open(p) }},
		{3, "a: 1\na(5): 2\n##    Format   3\n", func(p string) (shclw.Doc, error) { return shclw3.Open(p) }},
	} {
		dir := t.TempDir()
		path := filepath.Join(dir, "c.shcl")
		writeFile(t, path, c.text, 0o644)
		d, err := c.open(path)
		if err != nil {
			t.Fatal(err)
		}
		d.Remove("a")
		if err := d.Write(path); shclw.KindOf(err) != shclw.ErrRefused {
			t.Errorf("v%d: got %v, want refused", c.n, err)
		}
		if got := readFile(t, path); got != c.text || len(names(t, dir)) != 1 {
			t.Errorf("v%d: file %q, %v", c.n, got, names(t, dir))
		}
	}
}

// Open upgrades a 2.x file in memory and writes nothing. The first Write
// over it, by any name for the same file, backs up the original, and later
// ones don't.
func TestOpenUpgradesInMemory(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "c.shcl")
	writeFile(t, path, clean2x, 0o600)
	d, err := shclw3.Open(path)
	if err != nil || d.UpgradedFrom() != 2 || d.BackupName() != "" || !d.Rewrites() {
		t.Fatalf("open: %v, from %d, backup %q", err, d.UpgradedFrom(), d.BackupName())
	}
	if got := d.Strs("tags", nil); len(got) != 2 || got[1] != "b" {
		t.Errorf("tags read %q", got)
	}
	if readFile(t, path) != clean2x || len(names(t, dir)) != 1 {
		t.Fatalf("open wrote something: %v", names(t, dir))
	}
	if err := d.Write(filepath.Join(dir, ".", "c.shcl")); err != nil {
		t.Fatal(err)
	}
	b := d.BackupName()
	if filepath.Dir(b) != dir || readFile(t, b) != clean2x || !hasMode(t, b, 0o600) {
		t.Errorf("backup %q", b)
	}
	if got := readFile(t, path); !strings.HasPrefix(got, clean2to) || shclw3.FormatOf(got) != 3 {
		t.Errorf("file holds %q", got)
	}
	if d.Rewrites() {
		t.Error("still rewrites after the first save")
	}
	d.Put("n", 1)
	if err := d.Write(path); err != nil || len(names(t, dir)) != 2 || d.BackupName() != b {
		t.Errorf("second write: %v, %v", err, names(t, dir))
	}
	again, err := shclw3.Open(path)
	if err != nil || again.UpgradedFrom() != 0 || again.Int("n", 0) != 1 {
		t.Errorf("reopen: %v, from %d", err, again.UpgradedFrom())
	}
}

// A file that changed since Open read it is left alone, with no backup.
func TestOpenUpgradeEditedMeanwhile(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "c.shcl")
	writeFile(t, path, clean2x, 0o644)
	d, err := shclw3.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	writeFile(t, path, "tags: x, y\n", 0o644)
	if err := d.Write(path); shclw.KindOf(err) != shclw.ErrIO {
		t.Errorf("got %v, want io", err)
	}
	if readFile(t, path) != "tags: x, y\n" || len(names(t, dir)) != 1 || d.BackupName() != "" {
		t.Errorf("folder now holds %v", names(t, dir))
	}
}

// Something already at the backup's name is never written over. The save
// is ErrBackupTaken and the file stays as it was.
func TestOpenUpgradeBackupTaken(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "c.shcl")
	writeFile(t, path, clean2x, 0o644)
	d, err := shclw3.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	// The name has the time in it to the second. Go again on a tick.
	for try := 0; try < 3; try++ {
		name := shcl3.BackupFileName(path, 2)
		writeFile(t, name, "mine\n", 0o644)
		err := d.Write(path)
		if shcl3.BackupFileName(path, 2) != name {
			for _, n := range names(t, dir) {
				os.Remove(filepath.Join(dir, n))
			}
			writeFile(t, path, clean2x, 0o644)
			if d, err = shclw3.Open(path); err != nil {
				t.Fatal(err)
			}
			continue
		}
		if shclw.KindOf(err) != shclw.ErrBackupTaken || d.BackupName() != "" {
			t.Errorf("got %v, want backuptaken", err)
		}
		if readFile(t, path) != clean2x || readFile(t, name) != "mine\n" || len(names(t, dir)) != 2 {
			t.Errorf("file %q, backup %q, %v", readFile(t, path), readFile(t, name), names(t, dir))
		}
		return
	}
	t.Fatal("the clock ticked on every try")
}

const block2x = "#\r\n# This config file format is SHCL.\r\n# \"Simple Hierarchical Config Language\"\r\n" +
	"#    Home     https://github.com/jim-collier/shcl\r\n#\r\n"

// A save of a file with 2.x's block and no Format line puts the current block
// where it sat, with its Format line, so the next Open reads what was put
// rather than take the file as 2.x (2026101513330010).
func TestWriteSwapsOldBlock(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "c.shcl")
	text := "# app\r\nname: demo\r\n\r\n" + block2x + "\r\nport: 80\r\n"
	d := shclw3.Parse(text)
	if err := d.Put("dir", `C:\temp`); err != nil {
		t.Fatal(err)
	}
	if err := d.Write(path); err != nil {
		t.Fatal(err)
	}
	got := readFile(t, path)
	if got != d.Text() {
		t.Errorf("Text %q, file %q", d.Text(), got)
	}
	at := strings.Index(got, "##\r\n## This config file format is SHCL.")
	if shclw3.FormatOf(got) != 3 || strings.Contains(got, "# This config file format is SHCL.\r\n# ") ||
		at < 0 || at > strings.Index(got, "port:") || strings.Count(got, "\n") != strings.Count(got, "\r\n") {
		t.Errorf("file holds %q", got)
	}
	again, err := shclw3.Open(path)
	if err != nil || again.UpgradedFrom() != 0 || again.Str("dir", "") != `C:\temp` {
		t.Errorf("reopen: %v, from %d, dir %q", err, again.UpgradedFrom(), again.Str("dir", ""))
	}

	// A block's text in a raw body is the body's.
	raw := "tmpl:\n\t~~~\n" + strings.ReplaceAll(block2x, "\r\n", "\n") + "\t~~~\n"
	if err := shclw3.Parse(raw).Write(path); err != nil || readFile(t, path) != raw {
		t.Errorf("raw body: %v, %q", err, readFile(t, path))
	}
	// A Format line already says what the file is.
	stamped := strings.ReplaceAll(block2x, "\r\n", "\n") + "##    Format   3\n"
	if err := shclw3.Parse(stamped).Write(path); err != nil || readFile(t, path) != stamped {
		t.Errorf("stamped: %v, %q", err, readFile(t, path))
	}
}

// A file with no Format line or 2.x block that has errors on 3.0 and 2.0
// alike is current, with 3.0's errors in Diags. One marked as 2.x is still
// refused (2026101613005800).
func TestOpenFailsBoth(t *testing.T) {
	const both = "a: 1\nb: ```\nraw line\nc: 2\n"
	dir := t.TempDir()
	path := filepath.Join(dir, "c.shcl")
	writeFile(t, path, both, 0o644)
	d, err := shclw3.Open(path)
	want := shclw3.Parse(both).Diags()
	if err != nil || d.UpgradedFrom() != 0 || d.Rewrites() || d.Int("a", 0) != 1 ||
		len(d.Diags()) == 0 || d.Diags()[0] != want[0] {
		t.Fatalf("open: %v, from %d, diags %v", err, d.UpgradedFrom(), d.Diags())
	}
	if _, err := shclw3.OpenWith(path, shclw.Strict); shclw.KindOf(err) != shclw.ErrStrict {
		t.Errorf("strict open: %v", err)
	}
	if u, err := shclw3.Upgrade(both, 0); err != nil || u.Changed || u.From != 3 || u.Text != both {
		t.Errorf("upgrade 0: %v, %+v", err, u)
	}
	if u, err := shclw3.UpgradeFile(path, 0); err != nil || u.Changed || len(names(t, dir)) != 1 {
		t.Errorf("upgrade file: %v, %+v, %v", err, u, names(t, dir))
	}
	for _, c := range []struct {
		name string
		text string
		from int
	}{
		{"from 2", both, 2},
		{"2.x block", strings.ReplaceAll(block2x, "\r\n", "\n") + both, 0},
		{"Format 2", "##    Format   2\n" + both, 0},
	} {
		if _, err := shclw3.Upgrade(c.text, c.from); shclw.KindOf(err) != shclw.ErrRefused {
			t.Errorf("%s: upgrade %v, want refused", c.name, err)
		}
		if c.from != 0 {
			continue
		}
		writeFile(t, path, c.text, 0o644)
		if _, err := shclw3.Open(path); shclw.KindOf(err) != shclw.ErrRefused {
			t.Errorf("%s: open %v, want refused", c.name, err)
		}
	}
}
