<!-- markdownlint-disable MD007 -- Unordered list indentation -->
<!-- markdownlint-disable MD010 -- No hard tabs -->
<!-- markdownlint-disable MD033 -- No inline html -->
<!-- markdownlint-disable MD055 -- Table pipe style [Expected: leading_and_trailing; Actual: leading_only; Missing trailing pipe] -->
<!-- markdownlint-disable MD041 -- First line in a file should be a top-level heading -->
<!-- TOC ignore:true -->
# Design

Status: draft, 2026-10-15. Go, Rust, and C are built over shcl 3.0, with 2.0 built in for the upgrade's check. 3.0 is the floor. Python and shcl 4 are in the backlog.

<!-- TOC ignore:true -->
## Table of contents

<!-- TOC -->

- [Summary](#summary)
- [Background](#background)
- [Goals](#goals)
	- [Non-goals](#non-goals)
- [Is it worth it](#is-it-worth-it)
- [Layout](#layout)
- [API](#api)
	- [Loading and saving](#loading-and-saving)
	- [Reads](#reads)
	- [Walking a document](#walking-a-document)
	- [Writes](#writes)
	- [Format and upgrade](#format-and-upgrade)
	- [Errors](#errors)
	- [Everything else](#everything-else)
- [What the wrapper evens out](#what-the-wrapper-evens-out)
- [Ops scripts](#ops-scripts)
- [Testing](#testing)
	- [Shared cases](#shared-cases)
	- [Fuzzing](#fuzzing)
- [Sharing the code](#sharing-the-code)
- [Decisions](#decisions)
- [Open questions](#open-questions)
- [Copyright and license](#copyright-and-license)

<!-- /TOC -->

## Summary

- One small API over every shcl version a program might build against, the same in each language.

- A program picks its shcl version once, by import path in Go or cargo feature in Rust. Moving to another version is that one change, plus whatever its files need from `Upgrade`.

- The quirks each version has, and that each program used to work around on its own, are handled in one place.

- shcl 3.0 is the floor. The files programs wrote on 1.2 and 2.0 come forward through the upgrade, which is in every build. `Open` does it in memory, and the first `Write` keeps the original.

- The rest of shcl is still there, through `Inner()`.

## Background

- shcl went from 1.2 to 2.0 to 3.0 in a few months, and 4 is planned soon after 3.0. 3.0 changes the file format a bit: arrays went from `a, b` to `[a, b]`, and selectors from `[#0]` to `(0)`. 4 keeps the format and changes the API a lot.

- Programs using shcl are spread across Go, Rust, C, C++, and Python, and over 1.2, 2.0, and several unreleased 3.0 commits. Most of them have their own adapter code for the same few problems.

## Goals

- The basics every config user needs, with the same names and the same behavior in every language and on every shcl version.

- Small enough that a fix is easy to make in every language at once. Each language is one module.

- Every behavior pinned by a test that runs the same script in every language and every shcl version.

- A way to reach the full shcl of a given version.

### Non-goals

- Covering all of shcl. Raw blocks, datetimes, durations, sizes, merging, and schema generation stay with shcl, through `Inner()`.

- Building on shcl 1.2 or 2.0. A program on either moves to 3.0 when it moves onto the wrapper.

- Hiding the file format change. A file written for a newer format than the bound shcl's is `ErrTooNew`. An older one is upgraded, and `UpgradedFrom` tells the program so.

- Publishing to a package registry.

## Is it worth it

- Yes, at this size. shcl's 4 bindings are about 2.3 MB of source. The wrapper is about 1,000 lines per language, plus a test driver.

- Most of the cost is in testing: every language times every shcl version, with each version's pinned copy fetched and built.

- The wrapper can't help with what it doesn't cover. A program that leans on line-keeping saves or merges still has those calls to move by hand when shcl changes them.

- It would stop being worth it if it grew toward shcl's own size. The line is the basics plus whatever 2 or more programs need.

## Layout

| Language | Where                                  | Pick the shcl version by
| :---     | :---                                   | :---
| Go       | `source/go/shclw`, plus `shclw3`       | import path: `.../source/go/shclw3`
| Rust     | `source/rust`, crate `shclw`           | feature `shcl3`, then module `shclw::v3`
| C        | `source/c`, `shclw.h`, plus `shclw3.c` | the `shcl.h` the build includes, which has to be 3.0

- Go can't import one package or another by condition inside one file, so each shcl version gets its own small package. They have the same calls, checked by the compiler against the `shclw.Doc` interface. The shared types and the ops runner are in `shclw`.

- Rust has one crate. Each shcl version is a module behind a feature, and the `ShclDoc` trait makes the compiler check that every module has every call. A later version's feature can be on beside `shcl3`, for a tool that reads one version and writes another.

- A later version's code starts as a copy of the one before with its differences made, so a reader sees the whole adapter in one file.

- C has one header, `shclw.h`, which includes the program's own `shcl.h` and checks from what it declares that it's 3.0. `shclw.c` has what every version shares, and `shclw3.c` is the adapter, as Go has a package. A program builds `shclw.c`, `info.c`, and `shclw3.c`. Built against another version, the header stops the build with an error. `run.c` has `shclw_run` and is only needed for ops scripts.
	- The program also builds `shclw3_v2.c` and `shclw3_v2_impl.c`, with shcl 2.0's `shcl.h` on their include path, for the upgrade check.
	- The header is usable from C++, and the build compiles every C file as C++ too.

## API

Go names are listed. Rust uses the same words in snake case, and `ShclDoc` has them as methods. C uses snake case with a `shclw_` prefix, and takes the document first: `shclw_int(d, "port", 8080)`, `shclw_get_str(d, path)`.

### Loading and saving

| Call                      | Does
| :---                      | :---
| `New()`                   | An empty document.
| `Parse(text)`             | Never fails. Problems are in `Diags()`.
| `ParseWith(text, level)`  | `Loose`, `Standard`, or `Strict`. Only `Strict` fails, with `ErrStrict`.
| `Open(path)`              | Reads and parses a file, and upgrades an older one in memory. A missing file is `ErrNotFound`, and a newer format is `ErrTooNew`.
| `OpenWith(path, level)`   | `Open` at a strictness.
| `d.Text()`                | The document as it would be saved.
| `d.Canonical()`           | The document in canonical form.
| `d.Write(path)`           | Saves `Text()` atomically. The first one over a file `Open` upgraded backs it up first.
| `d.Rewrites()`            | Whether `Write` puts the canonical form in place of the lines the document was loaded from.
| `d.UpgradedFrom()`        | The format `Open` upgraded the file from, or 0.
| `d.BackupName()`          | Where the first `Write` over an upgraded file put the original, or empty.
| `d.Diags()`, `d.Errors()` | The load's diagnostics, and how many are errors.
| `d.Check(schema)`         | Validates against a schema document.
| `schema.DropDeclared(ds)` | Drops the repeat hints for keys the schema says repeat.

- `Rewrites` is for a program that tells the user what a save will do. When it's true, lines no edit touched can change, and comments can move.
	- 3.0 keeps a file's own lines, and falls back to the canonical form when it can't keep them with the edits in. A child added under a flat dotted line like `b.c: 2` is one such edit.
	- A `New` document has no lines to keep, and says false.
	- On 3.0 it builds the save's text to find out, so it costs about what `Text` does.
	- `InfoBlock` and `StampFormat` don't change it. They touch only the block and the Format line, and the rest of the save is what it was.

- `Open` upgrades a file the way `Upgrade` does with `from` 0, adn nothing is written until a `Write`.
	- A file with no Format line that has 2.x's `#` info block is taken as 2.x. Otherwise one that loads clean on 3.0 is taken as current. One that loads with errors is taken as 2.x, upgraded when shcl 2.0 reads it clean, and `ErrRefused` with 2.0's errors when it doesn't. One that reads two ways is `ErrAmbiguous`. Either way the document is empty, as for `ErrTooNew`. In C the empty document's `shclw_diags` has 2.0's errors.
	- A file whose Format line names 3 is current, errors and all, and skips the upgrade.
	- Reads, `Lines`, `Diags`, and `Text` are all of the upgraded text, and the strictness applies to it.
	- The first `Write` over the file backs up the original as `UpgradeFile` does, with the same name and `ErrBackupTaken`. Over the file means the path `Open` read, or the same file by another name. A `Write` anywhere else makes no backup, and later ones over the file make none either. A file that changed since `Open` read it is left alone with `ErrIO`.
	- `Rewrites` is true until that first `Write`, since it replaces the original's lines.
	- `Parse` never upgrades. Text has no file to back up, and `Upgrade` is there for it.
	- A program whose files can't say they're older converts them ahead of time with `UpgradeFile(path, 2)`. To only read one, it passes `Upgrade(text, 2)`'s text to `Parse`.

- C also has `shclw_compact`, for a program that edits a lot between saves. [Decisions](#decisions) says why.

### Reads

- 2 tiers, as in shcl's own design.
	- `d.Int(path, fallback)` gives the value, or the fallback when there's no good one.
	- `d.GetInt(path)` gives a `Got` with `Value`, `Status`, `Line`, and `Text`, the value as typed.

- Types: `Int`, `Float`, `Bool`, `Str`, `Strs`, `Ints`, `Floats`, and `Raw`, the value as typed with no coercion.

- Statuses: `Good`, `Empty`, `NotFound`, `BadType`, `Multiple`, and `BadPath`.

- A float read never gives infinity or NaN. A value like `1e400` is `BadType`, and the fallback read gives the fallback.

- A key set more than once reads as `Multiple`. To take one of them instead, read through `First` or `Last`, which narrow the path to that one instance. Every type and both tiers work on it, and so do Put, Comment, and Remove. `Lines` says where each instance is, for a warning.

	~~~go
	port := d.Int(d.Last("port"), 8080)
	if lines := d.Lines("port"); len(lines) > 1 {
		log.Printf("port is set more than once; lines %v are ignored", lines[:len(lines)-1])
	}
	~~~

### Walking a document

| Call                            | Does
| :---                            | :---
| `d.Has(path)`                   | Whether the path names anything.
| `d.Count(path)`                 | How many instances it names.
| `d.Names(path)`                 | Child names under it, once each, in file order.
| `d.Each(path)`                  | Every instance, as a `Node` with `Path`, `Value`, and `Line`.
| `d.Child(base, name)`           | `base` with one more name on the end, quoted if it needs it.
| `d.Line(path)`                  | The line a single node is on.
| `d.Lines(path)`                 | The line of each instance, in file order.
| `d.First(path)`, `d.Last(path)` | `path` narrowed to its first or last instance in the file. With none it comes back as it is.

- A `Node.Path` reads that instance and no other, whatever selector syntax the bound shcl uses. A program builds on it with `Child` and never writes a selector itself. `First` and `Last` give the same kind of path.
	- Every step of the path that repeats gets an index, not only the last, so an edit takes the path too. For `tv.g` with `tv` written twice, the paths are like `tv(0).g` and `tv(1).g`.
	- A wildcard like `a.*.b` lists each node it reaches, with its line. A slot that reaches no node, or several, is left out, as 3.0 reads it. So `Each` can list fewer than `Count` says. Plain reads of a wildcard path still differ by version, and the wrapper leaves them as they are.

- `Each` lists instances in the order shcl keeps them, which isn't always the file's. A block written twice with the same name and value is one block, so `tv.f` in the second part comes after the first part's `tv.f`, even with another `tv` block between them. `Lines`, `First`, and `Last` go by line instead, which is the same on every version.
	- An instance a put made has no line. `Lines` gives it as 0, and `First` and `Last` count it after the ones from the file.
	- A canonical save folds those blocks together, so after the save and a reload, the last `tv.f` can be another one. `Rewrites` says whether a save will be canonical.

	~~~go
	for _, t := range d.Each("table") {
		name := d.Str(d.Child(t.Path, "name"), "")
	}
	~~~

### Writes

| Call                      | Does
| :---                      | :---
| `d.Put(path, v)`          | Sets an int, float, bool, string, or a slice of strings, ints, or floats.
| `d.PutSyntax(path, text)` | Sets a value written in shcl's own syntax. The whole text has to read back, so one with a trailing comment is `NoReadBack`.
| `d.Comment(path, text)`   | Sets the comment above a path.
| `d.Remove(path)`          | Takes out a path and everything under it, and says how many instances went. That's every instance, and also the ones under any step that repeats.

- A Go `float32` or Rust `f32` is written as its own shortest digits, so 0.9 is written `0.9` and not as the float64 it widens to. C has `shclw_put_float32` for a `float`.
	- A tie between 2 spellings goes to the even one, except at a float32 power of 2, where Go rounds up. Rust and C give the same digits as Go.

- Infinity and NaN are refused with `BadValue`, and nothing changes, since no read would take them back.

- C has no `any`, so it has a put per type, `shclw_put_int` to `shclw_put_floats`. `shclw_each` gives `shclw_node`s, and `shclw_child` takes the name with its length, so a name from `shclw_names` goes in as it came out.

### Format and upgrade

- `FormatOf(text)` is the format a file's `##    Format   N` line names, or 0.

- `Upgrade(text, from)` and `UpgradeFile(path, from)` move a file to the bound shcl's format. `Open` does the same in memory, as [Loading and saving](#loading-and-saving) says, and `UpgradeFile` is for converting ahead of time.
	- With `from` 0 the Format line decides. A file with none is taken as 2.x when it has 2.x's `#` info block, and otherwise as current. When it's left as it is, `From` is the bound format.
	- `from` 2 says the file is known to be from shcl 2.x or older. The file can't say so itself, so only the program can.
	- `UpgradeFile` copies the original beside it first, and `Backup` names the copy.
	- `UpgradeFile` reads the file once. The 2.0 check, the conversion, and the backup all use that text.
	- A file that changed since that read is left alone with `ErrIO`. It's checked before the backup and again right before the write. That's a check and not a lock, so an edit in the moment between the last check and the rename is still lost.
	- The new text goes to a temp file that's renamed into place, so a failed write leaves the original. If a failed replace left nothing at the path, the backup is moved back. The backup is removed whenever the original is still there, and only kept, and named in the error, when it can't be put back.
	- A file whose Format line names a newer format is `ErrTooNew`, as `Upgrade` says, and anything that isn't a regular file is `ErrIO`.
	- A file taken as 2.x that shcl 2.0 reports errors on is `ErrRefused`, with those errors in the error's `Diags`. Nothing is written and no backup is made. 2.0 read such a line the way it repaired it, or not at all, and a raw block that never closes took the rest of the file. The upgrade would make a clean file out of it that means something else.
	- A 2.x file comes out reading the values 2.0 read. shcl's `migrate` leaves a 2.x backslash escape as typed, and 3.0 reads a backslash as text, so `C:\\new` read as `C:\new` in 2.x and would read as `C:\\new` after. The upgrade sets each value with an escape in it again, so 3.0 reads what 2.0 read. Then every path 2.0 reads one value at is read back against 2.0.
	- 1.2 read values the same way, but read names as typed, as 3.0 does. A name 2.0 read with an escape resolved can't be renamed, so it's refused, unless the file's Format line names 1.
	- A value that still reads differently, or such a name, is `ErrRefused`, with the lines in `Diags`. Nothing is written.
	- That check needs shcl 2.0, so the 3.0 adapter builds it in too. In Rust the `shcl3` feature turns on the `shcl2` dependency.
	- In C, 2.0's `shcl.h` and 3.0's declare the same names, so 2.0's goes in 2 files of its own with each call renamed. The C upgrade gives 2.0's errors in its result, `shclw_upgraded`, since C's error has no list.
	- 2.x's `#` info block is swapped for the current one where it sits, in a file with no Format line. shcl leaves a clean file's block alone, and puts the new block at the end of a file it writes out again.
	- When shcl writes a CRLF file out again with LF only, the upgrade puts the CRLF back, unless that would change a value.
	- The files 1.2 and 2.0 wrote come forward this way. One with no Format line or 2.x info block that loads clean on 3.0 is taken as current, so a program taht knows its files are older passes `from` 2. A file the wrapper's own 1.2 or 2.0 adapter wrote has an info block naming format 1 or 2, and the upgrade puts 3.0's in its place.

- `d.InfoBlock(on)` puts the info block at the end of what the save writes, or with `on` false leaves it out.
	- Every old block comes off first, wherever it is. That's each spelling shcl has written, 2.x's `#` one, and a lone Format line with the note `migrate` writes under it. A Schema line in a block stays where it is.
	- shcl 3.0's own `SetBanner` leaves a block above the first field and adds a second one. The wrapper takes that one off too.
	- The block is shcl's `GenBanner`.
	- It goes last, after one blank line, so fields put after the call go above it.

- `d.StampFormat()` adds the Format line at the end of what the save writes, without the block, unless a comment already names the bound format. It goes right under the last line, same place as shcl's `Migrate` puts it.

- Both change what `Text`, `Canonical`, and `Write` give, and leave the document alone. Reads, `Lines`, and `Diags` are what they were.

- Both refuse a file whose Format line names another format. A newer one is `ErrTooNew`. An older one is `ErrRefused`, since stamping it would make `Upgrade` take it as current and skip the conversion. It has to be upgraded first.
	- A file that ends in a raw block that never closes is `ErrRefused` too, since anything added would be the block's content.
	- An edit after the call can lead to the same refusal. Then `Text` leaves the block and the line out, and `Write` refuses adn writes nothing.

- A run of lines counts as a block, or as a Format line, only when taking it out leaves every value and every error the same. That's how a block's text inside a raw body is told from a real one, in every version.

- Lines the wrapper adds end the way most of the file's lines do, LF on a tie, as shcl's stamp does.

### Errors

- One error type, `*shclw.Error` in Go and `shclw::Error` in Rust, with a kind a program can act on: `NotFound`, `IO`, `Strict`, `Refused`, `BadPath`, `BadValue`, `TooNew`, `Unsupported`, `Ambiguous`, `BackupTaken`, and `NoReadBack`.

- `Refused` is a save that would drop lines the load couldn't use, which 3.0 refuses. It's also an upgrade of a 2.x file that shcl 2.0 reports errors on.

### Everything else

- `d.Inner()` is the shcl document underneath. Code that uses it is tied to that shcl version, which is the price of reaching past the wrapper.
	- In Go, shcl's own types are named by importing the same shcl module, which Go resolves to the version the wrapper uses.
	- In Rust, each module has the shcl crate under it as `shcl`, such as `shclw::v3::shcl::SetStatus`.

- `ShclVersion` and `FormatMajor` say what's underneath.

- `shclw.Run` takes an ops script, described below. It's the test driver's engine, and also a way to run a list of edits written as text.

## What the wrapper evens out

Each row has a test, either a shared case or an API test.

| Behavior                                 | shcl did                                                    | The wrapper does
| :---                                     | :---                                                        | :---
| `Names` with a name given twice          | Lists it once per line.                                     | Lists it once. Names match without ASCII case only, as in shcl, so `É` and `é` are 2.
| A put that doesn't read back the same    | Says ok.                                                    | Reads every put back. A mismatch is `NoReadBack`, with the edit left in place.
| `PutSyntax` text with a trailing comment | Drops the comment and says ok.                              | `NoReadBack`, with the edit left in place.
| Opening a newer format's file            | 3.0 reads it, with a hint.                                  | `ErrTooNew`. A Format line in a raw body doesn't count, as in 3.0.
| Opening a 1.2 or 2.0 file                | 3.0 reads it by its own rules, with errors.                 | `Open` upgrades it in memory, and the first `Write` backs up the original.
| Upgrading a 2.x file 2.0 had errors on   | 3.0 writes a clean file, with nothing lost and no error.    | Refuses with `ErrRefused` and leaves the file alone.
| A 2.x backslash escape in a value        | 3.0 keeps it as typed, and reads it as text.                | The upgrade writes it so 3.0 reads what 2.x read.
| An old info block above the first field  | 3.0's `SetBanner` leaves it and adds a second block.        | `InfoBlock` takes it off.
| Upgrading a file with 2.x's info block   | 3.0 moves it to the end, or leaves it when nothing changed. | Swaps in the current block where the old one was.
| Upgrading a CRLF file 3.0 writes again   | 3.0 writes LF.                                              | Keeps CRLF.
| A Format line without the block          | 3.0 has it only as part of `Migrate`.                       | `StampFormat`.
| A file that isn't UTF-8                  | Varies by language.                                         | `ErrIO`, everywhere.
| Walking repeated entries                 | A program writes selectors itself, and 3.0 changed them.    | `Each` gives paths, and `Child` extends them.
| A save that can't keep the file's lines  | 3.0's save returns a flag, once it's done.                  | `Rewrites` asks before the save.

- `PutSyntax` checks its text by setting it in a new document and parsing `k: text` in another. The two have to come out the same, and the value at the path has to read the same as the parsed one.

- An empty array reads back as an empty `Good` on 3.0. The read-back check also takes `Empty`, which is how 1.2 and 2.0 read one.

## Ops scripts

An ops script is one op per line, with fields split by tabs. `\t`, `\n`, `\r`, and `\\` in a field are a tab, newline, return, and backslash. Blank lines and lines starting with `#` are skipped.

| Op                                      | Prints
| :---                                    | :---
| `parse [loose\|standard\|strict]`       | `ok` or `error strict`, and the document is reloaded
| `text`, `canonical`                     | the document, one `\|`-prefixed line per line
| `rewrites`                              | `true` or `false`, from `Rewrites`
| `diags`, `errors`                       | each diagnostic as line, severity, code, and message, or the count
| `get TYPE PATH`                         | status, line, and the value. Arrays print a count, then the items.
| `or TYPE PATH FALLBACK...`              | the value or the fallback
| `has`, `count`, `line`, `names`         | the answer for PATH
| `lines PATH`                            | how many instances, then each one's line
| `first TYPE PATH`, `last TYPE PATH`     | `get` of the first or last instance
| `each PATH`                             | index, line, and value, one instance per line
| `eachget TYPE PATH CHILD`               | `get` of CHILD under each instance
| `put TYPE PATH VALUE...`                | `ok` or `error KIND`
| `syntax PATH TEXT`, `comment PATH TEXT` | `ok` or `error KIND`
| `remove PATH`                           | how many instances went
| `info on\|off`, `stamp`                 | `ok` or `error KIND`, from `InfoBlock` and `StampFormat`
| `check FILE [all\|drop]`                | the schema check, with the load's own diagnostics for `all`, and then dropped ones for `drop`
| `format`, `upgrade [FROM]`              | the format number, or the upgrade's result and text
| `write FILE`                            | `ok` and the saved file, or `error KIND`
| `open FILE [loose\|standard\|strict]`   | `ok`, or `error KIND` and the error's diagnostics, and the document is reloaded
| `upgraded`                              | `UpgradedFrom` and `BackupName`, then the backup's text
| `files`                                 | the names in the folder

- TYPE is `int`, `float`, `bool`, `str`, `strs`, `ints`, `floats`, or `raw`.

- A float field takes decimal and exponent forms, plus `nan`, `inf`, and `-inf`. A number too big for a float is `inf`. Each language's own parser takes other spellings, which the ops leave out so both read a script the same.

- Every op is echoed first, after a `>` and a space, so an output file reads as a transcript.

- Floats print the same in every language: the shortest digits that read back exactly, positional from 1e-5 to 1e17, otherwise with an exponent. When 2 spellings of that length are equally close, the even one wins, as in Go.

- `check`, `write`, `open`, and `files` only work in the folder the driver is given, and refuse a name with a path in it. A case's own files are in that folder, `doc.shcl` among them. The time in a backup's name prints as `STAMP`.

## Testing

### Shared cases

- Each folder in `tests/cases` is a document, an ops script, and one `want.vN.txt` per shcl version. Go and Rust both run every case on every version and compare with the same want files, so Go and Rust can't drift apart.

- A file named `x.vK.ext` stands in for `x.ext` from version K up, for a later version whose syntax differs. Every case has one set of files today, for 3.0.

- `034-upgrade-1x-written` and `035-upgrade-2x-written` hold files the wrapper's 1.2 and 2.0 adapters wrote, kept from before those were dropped, so the upgrade is still tested on them.

- `038` through `046` hold a real file each program wrote, named by its stand-in: a committed default, a sample config, or a fixture from its tests, with project names and anything personal taken out. Each case says what `Open` makes of it. Folderkite's is the output of its own test writer, built against 2.0.

- `SHCLW_UPDATE=1 go test ./shclw/` writes the want files. A changed want file is reviewed in the diff like code.

- Every feature and every fix gets a case, or a line in an existing one.

- C runs the same cases through its driver with `cicd/c_cases.py`, op by op against the same want files.

### Fuzzing

- `cicd/fuzz.py` makes a document and an ops script from each seed, with a generator that knows each format's syntax. Half of the generated documents are in the 1.2 and 2.0 syntax, as old programs wrote them, so the upgrade sees them. A quarter of the documents are mutated copies of the case documents instead.
	- Each driver gets the document as `doc.shcl` in its own folder. A quarter of the scripts `open` it first and end by saving over it twice, so an old one is upgraded in memory and backed up.

- Each case runs through the Go, Rust, and C drivers on the same shcl version. Rust's and C's outputs must match Go's byte for byte, apart from the time 3.0 stamps on a line it comments out. None may crash, and a put that says ok must read back the same. A float read that says `Good` must not give infinity or NaN. A `first` or `last` read must not say `Multiple`, and has to be on the first or last line `lines` gave for that path. An edit that says ok must have found one instance at every step of its path that exists, counted just before it. The text right after an `info on` that says ok ends with the block naming the version's format, holds one block where the document had no block text in a raw body, and a second `info on` doesn't change it.

- It runs for a wall-clock budget, and a failing seed is saved with its document, script, and each driver's output. If too few reads find a value, the run fails, since a generator that stopped making readable documents would pass forever.

- C's driver is the sanitized build, so a leak or a bad read fails the case. The fuzz runs in the C container, where the C drivers are built. Without the pinned C headers it fuzzes Go against Rust alone.

- Go's own fuzzer runs `Run` on random documents and scripts against every version, looking for panics.

- `cicd/cicd.bash` runs format, lint, tests, the C leak checks, and both fuzzers. `--ci` runs format check, lint, and tests, and is the pre-push hook. `--quick` leaves out the C sanitizers, analyzers, and leak checks, and anything that needs the network, and fuzzes for 10s.

## Sharing the code

- Go: `go get github.com/jim-collier/shclwrap/source/go@<commit>`, then import the one version package. Copying `shclw` and the version package into a project works too.

- Rust: a git dependency on this repo with one feature on, or a copy of `source/rust/src`.

- C: a copy of `shclw.h`, `shclw_adapter.h`, `shclw.c`, `info.c`, and `shclw3.c`, plus `run.c` for ops scripts. The program builds `shcl.h`'s own implementation once, as shcl asks. It also takes the `shclw3_v2` files and shcl 2.0's `shcl.h`.

- A fix made in a copy goes back here, with a case, so every program gets it.

## Decisions

- The wrapper has its own small set of names. Some of its ideas come from shcl 4's design, but its names don't, since 4 is a draft that may still change.

- shcl 3.0 is the floor, and 4 comes after it once it exists. Programs on 1.2 and 2.0 move to 3.0 as they move onto the wrapper, and their files come forward through the upgrade, which is in every build. That replaced covering 1.2 and 2.0 too, which kept 2 more adapters per language for programs that were leaving them anyway (2026101613005800).
	- shcl 2.0 stays built in beside 3.0 in every language, for the upgrade's check.

- Go has one package per shcl version. Build tags were the other choice, but a build that leaves out the tag compiles against another shcl with no error.

- Rust uses one crate with a module per version over separate crates, so a program that needs 2 versions at once can have both.

- Each version's adapter is a full copy with its differences, over one generic adapter with hooks. Most of each adapter is the differences.

- A put is always read back. shcl 4 plans the same check inside its setters, and until then the wrapper is the one place to do it for every version.

- A put of infinity or NaN is refused with `BadValue` in every version, as 3.0 does. Writing one and then saying `NoReadBack` was the other choice, but it leaves a value in the file that no read takes.

- An upgrade refuses a 2.x file that shcl 2.0 reports errors on, rather than count those lines in `Unsure`. A count is easy to skip over, and the upgraded file loads clean, so nothing would point at those lines again. It's the same answer shcl's own `migrate` gives a raw block that never closes.

- The upgrade writes each 2.x value so 3.0 reads what 2.0 read, rather than keep `migrate`'s backslashes as typed, which read as text now. A line it can't carry that way is refused, rather than counted in `Unsure`, as with 2.0's errors, and `Open` doesn't show `Unsure` at all (2026101910455900).
	- So a 3.0 file with no Format line and an error, which `Open` takes as 2.x, has its backslash escapes read the 2.x way too. A program that writes its files with `StampFormat` or `InfoBlock` never has that happen.
	- A file `Open` takes as current keeps its backslashes as typed, even one 2.x wrote, when nothing in it says so.

- `Open` upgrades an old file in memory, rather than leave it to the program to call `UpgradeFile` first. A program that only reads then never writes, and one that saves gets the backup on its first `Write` (2026101613005800).
	- It upgrades as `Upgrade` with `from` 0 does, never 2. shcl says a program calling its upgrade on every start must not pass 2, since a 3.0 file that picks up an error would then be read by 2.x rules.
	- So a 3.0 file with no Format line and an error is taken as 2.x, and is upgraded or refused. A program that writes its files with `StampFormat` or `InfoBlock` never has that happen.
	- Reads, `Lines`, `Diags`, and `Text` are the upgraded text's, rather than the original's line numbers. shcl gives no line map, the upgraded text is what the save writes and the next `Open` reads, and the original's errors are why it was upgraded.
	- `Parse` of text never upgrades. There's no file to back up, and `Upgrade` already takes text.
	- `UpgradedFrom` and `BackupName` say what happened, rather than an `Upgraded` value on the document. Its text would be the document's own, and its count of unsure lines is from `UpgradeFile`, for a program that wants it ahead of time.
	- `Rewrites` is true until the first `Write` over the file, since that one replaces every line the file had.
	- The backup goes on the first `Write` over the file `Open` read, by path or the same file by another name, rather than on any `Write`. A copy saved somewhere else replaces nothing.
	- It uses `UpgradeFile`'s backup and replace code, so a file that changed since `Open` read it is `ErrIO`, and the backup name and `ErrBackupTaken` are the same.
	- An `Open` that's refused or ambiguous gives an empty document, as for `ErrTooNew`, rather than the original read by 3.0's rules. In C, 2.0's errors are that document's diagnostics, since C's error has no list.
	- Rust's `write` takes `&mut self`, since it records the backup.

- With `from` 0, a file with no Format line that has 2.x's `#` info block is taken as 2.x, rather than as current (2026101513330010). No 3.0 writer makes that block, and Quillroute checked for it the same way.
	- A 3.0 program that saves over a 2.x file without upgrading it keeps the block, and a backslash escape it writes is then read the 2.x way. No program on 3.0 is known to do that. The wrapper's own `Parse` followed by `Write` does, so this call waits on an answer.

- `UpgradeFile` on 3.0 does its own read, backup, and write with shcl's parts, rather than call shcl's `UpgradeFile`. shcl's reads the file again after the wrapper's 2.0 check, so an edit in between was converted with no check.

- A file that changed under `UpgradeFile` is `ErrIO`, as shcl reports it, rather than a kind of its own.

- `Rewrites` is a call made before `Write`, rather than a second result from `Write`. It sits beside `Text` and `Canonical`, a program can put it in a plan before anything is written, and `Write` keeps one result in every language.

- The first or last of a repeated key is picked by path, with `First(path)` and `Last(path)`, rather than by a mode on each read or a setting on the document. One pair covers all 8 types in both tiers, and the edits too, the same way `Each` paths do. A setting would change what every read means from somewhere else in the program. A plain read still says `Multiple`, so taking one is always asked for at the call.

- `First` and `Last` go by line, the order the file was written in, since that's what "the last one wins" means to whoever wrote it. `Each` order differs from it for a block written in parts.

- An edit is refused when any step of its path has several instances, not only the last, since that's where 3.0 refuses. A test walks every edit on the `Doc` interface, so a new setter can't skip it.

- `Remove` takes every instance, even where the path repeats at an inner step, on every version. It says how many went, so nothing is picked for the caller. The setters can't do that, since there's one value to set.

- `Each` narrows every step that repeats, so its paths take edits too. `First` and `Last` give that path even for one instance, since a path as typed can repeat at an inner step and be refused.

- `Each` on a wildcard lists the nodes it reaches, as 3.0's own field list does, rather than refusing wildcards.

- The info block names the bound shcl's format, so the 1.2 and 2.0 adapters wrote format 1 and 2. A block naming 3 there would have made 3.0's `Upgrade` take an unconverted 2.x file as current.

- `InfoBlock` and `StampFormat` take effect when the save's text is built, rather than editing the document. 3.0 has no call for a lone Format line. Parsing the edited text again would renumber the lines and lose the load's diagnostics and its count of lost lines, which `Write` refuses on.

- One line-based routine does the block in every version, rather than 3.0's `SetBanner` plus a fix for what it misses. Every version then gives the same text, and Go and Rust share the cases.

- The upgrade puts the new block where 2.x's was by swapping the old block for a comment line before shcl's upgrade, and the block for that line after it. shcl gives no line map, and a comment stays where it was through shcl's rewrite. Where the file already has that line, the block goes at the end (2026101513330011).

- 2.x's `#` block comes off on every version. shcl 3.0 takes it off only in `Upgrade`, and reads it otherwise as a comment of the file's own. Nobody writes that title line by hand, and leaving it gives the file 2 blocks.

- A file whose Format line names another format is refused, rather than written over. An older one means the file still needs `Upgrade`, and a newer one is a file this shcl can't read right.

- `Lines` is its own call, rather than a field on `Got`, so a program that keeps the `Multiple` default can still name the lines in its warning, and a read costs nothing extra.

- MIT, since programs copy this code into themselves.

- C hands everything back in memory the caller owns, and frees with `free()`: strings, arrays, the text, and diagnostics, each as one block. shcl keeps what each read gives out until the document is freed, and a program reading in a loop grew without end, so every C read copies its answer out and gives shcl's read memory back before it returns. "Valid until the next call" was the other choice. It costs no copy, but a second read quietly breaks the first one's pointer.
	- So a result from shcl's own read calls on `shclw_inner(d)` is good only until the next shclw call. That's the price of mixing the two.
	- Edits grow shcl's write memory the same way, until `shcl_compact`. `shclw_write` compacts after a good save, so a program that edits and saves in a loop stays level. A save is where a round of edits ends, and it already costs a pass over the document. Compacting on every edit would cost that pass each time. A program that edits in a loop and never saves can call `shclw_compact`. It's C's own, as `shcl_compact` is shcl's, since Go and Rust free that memory themselves.

- Every C string handed back has its length and a NUL after it. A value can hold a NUL, so the length is the one to go by, and the NUL is there so a string can go straight to a C call that wants one. Values and document text going in take a pointer and a length. Paths and file names are plain C strings, since a path with a NUL in it means nothing to shcl or the OS.

- C's `got` has no `text` field, the value as typed. shcl's C binding has no call that gives it, and building it from the line breaks after an edit. It can come back if shcl adds one.

- A C call that can fail returns a `shclw_kind`, `SHCLW_OK` on success, with details in a `shclw_error` out-parameter that may be NULL. The kinds are Go's in the same order, plus `SHCLW_ERR_NO_MEMORY`, which only C can see. A call that makes a document still gives one back on failure, empty for a file it couldn't read, as Go does.

- C checks the shcl version at compile time, from what `shcl.h` declares, since that's the only place C can see it. 3.0 is the first `shcl.h` with a version macro, `SHCL_FORMAT_MAJOR`, so a 1.2 or 2.0 `shcl.h` stops the build with a message.
	- Before 3.0 was the floor, a program on 1.2 or 2.0 set `SHCLW_SHCL` to say which, since their headers define the same macros. That macro is gone.

- C builds shcl 2.0 in beside 3.0 for the upgrade check, as Go and Rust do, rather than leave the check out of C. 2.0's calls are renamed by macros ahead of its `shcl.h`, in 2 files that never see 3.0's, so shcl isn't edited, and a build check fails on any 2.0 name left as it was.
	- A 3.0 program then needs 2.0's `shcl.h` to build, and its code grows by 2.0's parser.
	- 2.0 ends the program when memory runs out, where 3.0 has a hook for that.

- The C upgrade gives one block, `shclw_upgraded`, and shcl 2.0's errors on a refused file are in it, rather than on the error. `shclw_error` is a fixed struct the caller never frees, and a list on it would need freeing after any call that can fail.

- C's `shclw_drop_declared` takes the document and drops hints from its own diagnostics, rather than taking a list as Go and Rust do. shcl's C binding has only that form, and the match is on hint text that shcl builds inside itself.

- The build gets `shcl.h` by commit, checked by sha256, into a cache the repo ignores, rather than keeping a copy of each version in the repo. That's how Go and Rust get theirs too, through their package managers. The pins are in `source/c/shcl-pins.env`.

## Open questions

- When shcl 3.0's first beta is cut, the Go module path is still `.../v2`. Go won't take a `v3.0.0-beta1` tag on a `/v2` path, so the pin stays a commit until shcl's module path changes.

- shcl 4's API is a draft. Its adapter waits for 4 to exist in each language, which is planned Rust first, then C, then Go and Python.

## Copyright and license

> Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]<br />
> Licensed under the [MIT License](https://mit-license.org/)<br />
> SPDX-License-Identifier: `MIT`
