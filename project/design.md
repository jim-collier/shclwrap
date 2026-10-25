<!-- markdownlint-disable MD007 -- Unordered list indentation -->
<!-- markdownlint-disable MD010 -- No hard tabs -->
<!-- markdownlint-disable MD033 -- No inline html -->
<!-- markdownlint-disable MD055 -- Table pipe style [Expected: leading_and_trailing; Actual: leading_only; Missing trailing pipe] -->
<!-- markdownlint-disable MD041 -- First line in a file should be a top-level heading -->
<!-- TOC ignore:true -->
# Design

Status: draft, 2026-10-15. Go, Rust, C, and Python are built over shcl 3.0, with 2.0 built in for the upgrade's check. 3.0 is the floor. shcl 4 is in the backlog.

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

- Covering all of shcl. Raw blocks, datetimes, durations, sizes, merging past `OpenLayers`, and schema generation stay with shcl, through `Inner()`.

- Building on shcl 1.2 or 2.0. A program on either moves to 3.0 when it moves onto the wrapper.

- Hiding the file format change. A file written for a newer format than the bound shcl's opens read-only, and `Write` on it is `ErrTooNew`. An older one is upgraded, and `UpgradedFrom` tells the program so.

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
| Python   | `source/python`, module `shclw`        | the `shcl.py` found when it loads, which has to be 3.0

- Go can't import one package or another by condition inside one file, so each shcl version gets its own small package. They have the same calls, checked by the compiler against the `shclw.Doc` interface. The shared types and the ops runner are in `shclw`.

- Rust has one crate. Each shcl version is a module behind a feature, and the `ShclDoc` trait makes the compiler check that every module has every call. A later version's feature can be on beside `shcl3`, for a tool that reads one version and writes another.

- A later version's code starts as a copy of the one before with its differences made, so a reader sees the whole adapter in one file.

- C has one header, `shclw.h`, which includes the program's own `shcl.h` and checks from what it declares that it's 3.0. `shclw.c` has what every version shares, and `shclw3.c` is the adapter, as Go has a package. A program builds `shclw.c`, `info.c`, and `shclw3.c`. Built against another version, the header stops the build with an error. `run.c` has `shclw_run` and is only needed for ops scripts.
	- The program also builds `shclw3_v2.c` and `shclw3_v2_impl.c`, with shcl 2.0's `shcl.h` on their include path, for the upgrade check.
	- The header is usable from C++, and the build compiles every C file as C++ too.

## API

Go names are listed. Rust uses the same words in snake case, and `ShclDoc` has them as methods. Python uses Rust's words, except that a read with a fallback ends in `_or`, as in `int_or("port", 8080)`, so no method takes a builtin's name. `Upgraded.From` is `from_`, and Python raises `shclw.Error` where Go returns one. C uses snake case with a `shclw_` prefix, and takes the document first: `shclw_int(d, "port", 8080)`, `shclw_get_str(d, path)`.

### Loading and saving

| Call                      | Does
| :---                      | :---
| `New()`                   | An empty document.
| `Parse(text)`             | Never fails. Problems are in `Diags()`.
| `ParseWith(text, level)`  | `Loose`, `Standard`, or `Strict`. Only `Strict` fails, with `ErrStrict`.
| `Open(path)`              | Reads and parses a file, and upgrades an older one in memory. A missing file is `ErrNotFound`, and a newer format opens read-only.
| `OpenWith(path, level)`   | `Open` at a strictness.
| `OpenLayers(layers...)`   | Reads defaults, a site file, a user file, and so on, lowest first, as one document. Edits and saves go to the last file.
| `d.Text()`                | The document as it would be saved.
| `d.Canonical()`           | The document in canonical form.
| `d.Write(path)`           | Saves `Text()` atomically. The first one over a file `Open` upgraded backs it up first. Over a file that changed on disk since `Open` read it, it's `ErrChanged`.
| `d.WriteWith(path, opts)` | `Write` with `WriteOpts`. `Force` writes over a file that changed, `Create` writes only a new file, `Backup` copies the file aside where the save would lose lines, and `Mode` is a new file's mode.
| `d.ReadOnly()`            | Whether the document came from a newer format, which `Write` refuses.
| `d.Rewrites()`            | Whether `Write` puts the canonical form in place of the lines the document was loaded from.
| `d.UpgradedFrom()`        | The format `Open` upgraded the file from, or 0.
| `d.BackupName()`          | Where a `Write` copied the file `Open` read, or empty.
| `d.Diags()`, `d.Errors()` | The load's diagnostics, and how many are errors.
| `d.Unused()`              | The lines the load couldn't use, once each.
| `d.Merged()`              | Each name shcl kept one copy of, with the lines of every copy.
| `d.Commented()`           | Each comment line that reads as a `key: value`, with the path it would set.
| `d.Check(schema)`         | Validates against a schema document.
| `schema.DropDeclared(ds)` | Drops the repeat hints for keys the schema says repeat.

- `Rewrites` is for a program that tells the user what a save will do. When it's true, lines no edit touched can change, and comments can move.
	- 3.0 keeps a file's own lines, and falls back to the canonical form when it can't keep them with the edits in. A child added under a flat dotted line like `b.c: 2` is one such edit.
	- A `New` document has no lines to keep, and says false.
	- On 3.0 it builds the save's text to find out, so it costs about what `Text` does.
	- `InfoBlock` and `StampFormat` don't change it. They touch only the block and the Format line, and the rest of the save is what it was.

- `Open` upgrades a file the way `Upgrade` does with `from` 0, adn nothing is written until a `Write`.
	- A file with no Format line that has 2.x's `#` info block is taken as 2.x. Otherwise one that loads clean on 3.0 is taken as current. One that loads with errors is taken as 2.x when shcl 2.0 reads it clean, and upgraded. One that 2.0 has errors on too is taken as current, with 3.0's errors in `Diags`, and a strict `Open` fails on it with `ErrStrict` as usual.
	- A file its Format line or its 2.x block marks as 2.x is `ErrRefused` with 2.0's errors when 2.0 has errors on it. A 2.x file that reads two ways is `ErrAmbiguous`. Either way the document is empty. In C the empty document's `shclw_diags` has 2.0's errors.
	- A file whose Format line names 3 is current, errors and all, and skips the upgrade.
	- Reads, `Lines`, `Diags`, and `Text` are all of the upgraded text, and the strictness applies to it.
	- The first `Write` over the file backs up the original as `UpgradeFile` does, with the same name and `ErrBackupTaken`. Over the file means the path `Open` read, or the same file by another name. A `Write` anywhere else makes no backup, and later ones over the file make none either. A file that changed since `Open` read it is left alone with `ErrChanged`, and no backup is made.
	- `Rewrites` is true until that first `Write`, since it replaces the original's lines.
	- `Parse` never upgrades. Text has no file to back up, and `Upgrade` is there for it.
	- A program whose files can't say they're older converts them ahead of time with `UpgradeFile(path, 2)`. To only read one, it passes `Upgrade(text, 2)`'s text to `Parse`.

- A file whose Format line names a newer format opens read-only (2026101513330014). It reads as 3.0 reads it, with 3.0's errors in `Diags`, and the strictness applies as usual.
	- `ReadOnly` says so, and `Write` refuses it anywhere with `ErrTooNew`, since a 3.0 save would write the newer file's lines as 3.0 sees them under a Format line that says otherwise. `Force` doesn't change that. Edits still work in memory, and so does `Text`.
	- It's the text that decides, so `Parse` of such text is read-only too. `Upgrade` and `UpgradeFile` still refuse it with `ErrTooNew`.
	- Before this, `Open` refused such a file. A program that never writes, like Folderkite, can now read one.

- A `Write` over the file `Open` read first checks it still holds what `Open` read, byte for byte, and leaves it alone with `ErrChanged` when it doesn't (2026101513330017). After a good `Write` over it, the check is against what that `Write` left.
	- It's the same check the upgrade's backup makes, before the backup and again right before the rename. It's a check and not a lock, so another program's save in the moment between the last check and the rename is still lost. It catches the usual case, 2 programs that each read, edit, and save a while apart.
	- Content only, not size or mtime, so a file saved again with the same bytes counts as unchanged, and a touch alone doesn't count. The document keeps the text it read to compare, about the file's size in memory.
	- Over the file means the path `Open` read, or the same file by another name. A `Write` anywhere else isn't checked.
	- A layered document's user file that wasn't there at load has to still not be there. Another program that made it meanwhile is `ErrChanged`.
	- `Parse` and `New` documents read no file, so there's nothing to check.
	- `WriteWith(path, WriteOpts{Force: true})` skips the check, for a program that means to replace whatever is there. On an upgraded file the first `Write` still backs up the original `Open` read. Rust takes `&WriteOpts { force: true, ..WriteOpts::default() }`, Python `write(path, force=True)`, and C a `shclw_write_opts` or NULL.

- `WriteOpts{Create: true}` writes only where nothing is there yet, and is `ErrExists` otherwise (2026101513330015). Of 2 programs creating one file at once, exactly one gets it, and the file is all of that one's text.
	- The text goes to a temp file beside it, synced, which is then hard-linked to the name and removed. A link fails when the name is taken, so the name never holds part of a file, and there's no moment between a check and the write. That's what Radixmill does. On Windows C moves the temp into place without replacing, which fails the same way, and Go, Rust, and Python link there too, as NTFS has hard links.
	- A filesystem with no hard links, such as FAT, gets an exclusive create of the name itself. The one-winner rule holds, but a crash part way leaves part of a file, which then blocks the next create.
	- Anything at the name counts, a dangling symlink too.
	- It never replaces, so there's no changed check, and `Force` has nothing to skip. A document `Open` read refuses with `ErrExists` over its own file, unless that file was removed since. A read-only document is still `ErrTooNew`, and nothing is backed up.
	- Rust and Python have it as `create`, and C as the `create` field.

- `WriteOpts{Mode: 0o600}` is the permission bits a file this `Write` creates gets, exactly, past the umask (2026101513330016). It's born private and set by chmod before it's in place, so it's never readable to anyone the mode leaves out.
	- A file that's already there keeps its own mode, as every save keeps it, since shcl's atomic write copies the old file's mode to the new one. So a program that wants an existing file's mode changed calls chmod itself.
	- A file a `Write` with no mode creates gets 0666 less the umask, as any create does. So does one made through a symlink that points at nothing yet, since the name is taken and shcl's write follows the link.
	- More than permission bits, such as setuid or sticky, is `ErrBadValue`, and nothing is written.
	- Windows has no Unix modes and ignores it.
	- In Go and C a mode of 0 means none was given. Rust takes `mode: Some(0o600)`, and Python `mode=0o600`.

- `WriteOpts{Backup: true}` writes a save that would lose lines the load couldn't use, after copying the file `Open` read aside, where a plain `Write` is `ErrRefused` (2026101513330019). That's what Cobblevale does on its first save over such a file.
	- The copy is the upgrade's backup, with the same name, format 3 in it, and `ErrBackupTaken`. `BackupName` names it. It's the text `Open` read, so it has the lines the save drops.
	- It's made only when the save would lose lines. A save that keeps them, or has none to lose, makes no copy.
	- Only over the file `Open` read, by path or by another name. A `Write` anywhere else, a `Create` one, and a `Parse` or `New` document have no file to copy, and are still `ErrRefused`.
	- The changed check still comes first, so a file that changed since `Open` read it is `ErrChanged`, with no copy. With `Force` the copy is still what `Open` read, as on an upgraded file.
	- Once a copy is made, later `Write`s don't refuse, anywhere, since the lines are in it and the file no longer has them. The first `Write` over an upgraded file counts too, since its backup is the original.
	- On an upgraded file the first `Write` makes the one backup, of the original, either way.
	- Rust and Python have it as `backup`, and C as the `backup` field, last in `shclw_write_opts`.

- A save whose text has no Format line has 2.x's `#` info block swapped for the current one, with its Format line, where the old one sat (2026101513330010). That's `Write`, `Text`, and `Canonical`.
	- It's the upgrade's swap, with the same block finder, so a block's text inside a raw body doesn't count. The lines end as the file's did.
	- So a 2.x file read with `Parse` and saved names format 3, and the next `Open` reads what the program had.

- `OpenLayers` reads its layers lowest first, such as built-in defaults, then a site file, then the user's file, and the document reads as their merge (2026101512024156). A layer is a file, or text the program has, like its defaults or this run's command line.
	- A later layer's value, array, or raw block takes the place of a lower one's, whole. Blocks merge name by name, so a user file that sets `db.pool` keeps the site's `db.host`. It's shcl's own merge.
	- A missing file is skipped, since a site or user file that isn't there is normal. Any other error opening a file is returned as `Open` returns it, with an empty document.
	- A layer in a newer format opens as `Open` opens it. A lower one reads, and a user file makes the document read-only.
	- Edits and saves go to the last file layer, the user's file. `Text`, `Canonical`, `Write`, `Rewrites`, `InfoBlock`, `StampFormat`, `UpgradedFrom`, and `BackupName` are that layer's, so a save writes what the user's file had plus the edits, and nothing from below. A missing user file is made by the first `Write`. With no file layer, the last layer takes the edits.
	- Text layers after the last file, like a command line, still win reads, and are never saved.
	- Reads, `Each`, `Lines`, `Check`, and `Inner` are of the merge, built again after each edit, so a removed key reads as the layer below has it. A line number is the line in whichever layer the value came from.
	- `Diags` and `Errors` are every layer's, in layer order. `Diag.Layer` is the layer's place in the list, from 1, and 0 on a document that isn't layered.
	- An edit takes its path as the edit layer reads it. So an index from `Each`, `First`, or `Last` on the merge can name another instance, or none, where a lower layer has the same key.
	- Rust takes `&[Layer::Text(..), Layer::File(..)]`. C's `shclw_open_layers` takes an array of `shclw_layer`, each a `file`, or with `file` NULL, a `text` and `len`.

- `Unused` is the lines with an error, once each, in order, as a list of `Spot`s, each a `Line` and a `Layer` as `Diag` has them (2026101513330018). It's for a warning like "lines 4 and 9 were skipped".
	- An error on no line has no line to give, so it's left out.
	- A strict load that fails still has its document, and `Unused` says the same on it.
	- In C it leaves out the 2.0 errors a refused `shclw_open` keeps in `shclw_diags`, since Go and Rust have those on the error and not on the document.

- `Merged` lists each copy of a name shcl folded into an earlier one, since the 2 had the same name and value (2026101513330009). shcl's H002 hint says so only when the copies are apart, so 2 blocks next to each other fold without a word, and the second one's instance is gone.
	- Each `Merge` has the kept copy's `Path`, as `Each` gives it, adn `Lines`, the kept copy's line and then each later copy's. A third copy adds a line to the same `Merge`.
	- Every level of a folded block is listed, as H002 does. A block with a child block that folded too gives 2.
	- A dotted line like `db.user: u` that only reopens `db` isn't a copy of `db`, so it isn't listed. A dotted line that writes `db.host` again with the same value is a copy of `db.host`.
	- A repeated key with another value isn't folded. It reads as `Multiple`, and `Lines` lists it.
	- Both say what the load found, so an edit doesn't change them. On a layered document they're each layer's own, with `Layer` set, and the folding the layers' own merge does is left out, since that's what layering is for.
	- `Merged` parses the loaded text twice more when it's called, so it costs about 2 loads.

- `Commented` lists each comment line that reads as one `key: value` once its `#` comes off, such as a setting a program leaves in its file at the default (2026101513330021). Each has its `Path`, the `Value` as typed, its `Line`, and `Default`, which says the line ends in a `## Default` note.
	- The path is shcl's own reading. The line is uncommented alone and the text parsed again, so it's in the block the line sits in by its indent, with an index on a step that repeats. A key that's also set in that block is listed under the plain path.
	- A `##` line is a note, and isn't listed. Neither is a line in a raw body, or one that adds an error or moves any other field once it's uncommented, such as the start of an array that never closes.
	- Prose that reads as a setting, like a 1.2 banner's `# SHCL: Simple Hierarchical Config Language.`, is listed too, so a program checks the path.
	- It's of the save's text, edits and all, and on a layered document the edit layer's. It parses the text once more for each commented line, so a file with many costs about that many loads.
	- C has it as `shclw_commented`, giving `shclw_commented_line`s, with `is_default` for `Default`.

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

- A `Put` or `PutSyntax` of a path that isn't set, where the file has a commented `key: value  ## Default` line for it, fills in that line, so the lines under it don't move (2026101513330020). 3.0 adds the key at the end of its block instead.
	- The `#` and the `## Default` note come off, and the value is the put's. Only a line with that note is filled in. A commented line without one, like `# size: 12  # for the laptop`, is left alone, and the put adds a line as usual.
	- Names match without ASCII case, as in shcl. Where several lines match, the first one in the file is filled in.
	- It's only where the save keeps the file's lines, as `Rewrites` says. A path that's already set is set where it is.
	- The text is parsed again with the line filled in, so a key an earlier put added gets its real line, where `Lines` gave it as 0. `Diags` and `Errors` stay what the load found.
	- A set shcl refuses, such as `PutSyntax` text that isn't a value, leaves the line commented and the document as it was. A put that doesn't read back keeps its edit, as any put does.

- C has no `any`, so it has a put per type, `shclw_put_int` to `shclw_put_floats`. `shclw_each` gives `shclw_node`s, and `shclw_child` takes the name with its length, so a name from `shclw_names` goes in as it came out.

### Format and upgrade

- `FormatOf(text)` is the format a file's `##    Format   N` line names, or 0.

- `Upgrade(text, from)` and `UpgradeFile(path, from)` move a file to the bound shcl's format. `Open` does the same in memory, as [Loading and saving](#loading-and-saving) says, and `UpgradeFile` is for converting ahead of time.
	- With `from` 0 the Format line decides. A file with none is taken as 2.x when it has 2.x's `#` info block, or when it has errors on 3.0 and shcl 2.0 reads it clean. Otherwise it's current, errors and all. When it's left as it is, `From` is the bound format.
	- `from` 2 says the file is known to be from shcl 2.x or older. The file can't say so itself, so only the program can.
	- `UpgradeFile` copies the original beside it first, and `Backup` names the copy.
	- `UpgradeFile` reads the file once. The 2.0 check, the conversion, and the backup all use that text.
	- A file that changed since that read is left alone with `ErrChanged`. It's checked before the backup and again right before the write. That's a check and not a lock, so an edit in the moment between the last check and the rename is still lost.
	- The new text goes to a temp file that's renamed into place, so a failed write leaves the original. If a failed replace left nothing at the path, the backup is moved back. The backup is removed whenever the original is still there, and only kept, and named in the error, when it can't be put back.
	- A file whose Format line names a newer format is `ErrTooNew`, as `Upgrade` says, and anything that isn't a regular file is `ErrIO`.
	- A file its Format line, its 2.x block, or `from` 2 marks as 2.x, and that shcl 2.0 reports errors on, is `ErrRefused`, with those errors in the error's `Diags`. Nothing is written and no backup is made. 2.0 read such a line the way it repaired it, or not at all, and a raw block that never closes took the rest of the file. The upgrade would make a clean file out of it that means something else.
	- A 2.x file comes out reading the values 2.0 read. shcl's `migrate` leaves a 2.x backslash escape as typed, and 3.0 reads a backslash as text, so `C:\\new` read as `C:\new` in 2.x and would read as `C:\\new` after. The upgrade sets each value with an escape in it again, so 3.0 reads what 2.0 read. Then every path 2.0 reads one value at is read back against 2.0.
	- 1.2 read values the same way, but read names as typed, as 3.0 does. A name 2.0 read with an escape resolved can't be renamed, so it's refused, unless the file's Format line names 1.
	- A value that still reads differently, or such a name, is `ErrRefused`, with the lines in `Diags`. Nothing is written.
	- That check needs shcl 2.0, so the 3.0 adapter builds it in too. In Rust the `shcl3` feature turns on the `shcl2` dependency.
	- In Python, 2.0's `shcl.py` is kept beside the wrapper as `_shclw_shcl2.py` and loads under that name, since both are `shcl`.
	- In C, 2.0's `shcl.h` and 3.0's declare the same names, so 2.0's goes in 2 files of its own with each call renamed. The C upgrade gives 2.0's errors in its result, `shclw_upgraded`, since C's error has no list.
	- 2.x's `#` info block is swapped for the current one where it sits, in a file with no Format line. shcl leaves a clean file's block alone, and puts the new block at the end of a file it writes out again.
	- When shcl writes a CRLF file out again with LF only, the upgrade puts the CRLF back, unless that would change a value.
	- `Lost` and `Rewritten` list lines of the original, from 1, so a program can point the user at them in the backup (2026101513330012, 2026101513330013).
		- `Lost` is each line with a value 2.0 read taht the upgraded file doesn't have at all, such as a 2.x comma list with lines under it. A value that comes through reading differently is refused instead, as above.
		- `Rewritten` is every other line the upgraded text doesn't have as typed, like `tags: a, b` written as `tags: [a, b]`, or 2.x's info block. A line that only moved isn't one, and lines the upgrade added aren't listed.
		- `Unsure` is still shcl's own count.
		- C has them as `lost` and `rewritten`, with `n_lost` and `n_rewritten`, in the result's one block.
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

- One error type, `*shclw.Error` in Go, `shclw::Error` in Rust, and `shclw.Error` in Python, with a kind a program can act on: `NotFound`, `IO`, `Strict`, `Refused`, `BadPath`, `BadValue`, `TooNew`, `Unsupported`, `Ambiguous`, `BackupTaken`, `NoReadBack`, `Changed`, and `Exists`.

- `Refused` is a save that would drop lines the load couldn't use, which 3.0 refuses, unless `WriteOpts.Backup` copies the file aside first. It's also an upgrade of a 2.x file that shcl 2.0 reports errors on.

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
| Opening a newer format's file            | 3.0 reads it, with a hint.                                  | Read-only, and `Write` is `ErrTooNew`. A Format line in a raw body doesn't count, as in 3.0.
| Opening a 1.2 or 2.0 file                | 3.0 reads it by its own rules, with errors.                 | `Open` upgrades it in memory, and the first `Write` backs up the original.
| Upgrading a 2.x file 2.0 had errors on   | 3.0 writes a clean file, with nothing lost and no error.    | Refuses with `ErrRefused` and leaves the file alone. With nothing to mark the file as 2.x, it's current.
| A 2.x backslash escape in a value        | 3.0 keeps it as typed, and reads it as text.                | The upgrade writes it so 3.0 reads what 2.x read.
| An old info block above the first field  | 3.0's `SetBanner` leaves it and adds a second block.        | `InfoBlock` takes it off.
| Upgrading a file with 2.x's info block   | 3.0 moves it to the end, or leaves it when nothing changed. | Swaps in the current block where the old one was.
| Saving a file with 2.x's info block      | 3.0 keeps the block, and adds no Format line.               | Swaps in the current block where the old one was.
| Upgrading a CRLF file 3.0 writes again   | 3.0 writes LF.                                              | Keeps CRLF.
| A Format line without the block          | 3.0 has it only as part of `Migrate`.                       | `StampFormat`.
| A file that isn't UTF-8                  | Varies by language.                                         | `ErrIO`, everywhere.
| Walking repeated entries                 | A program writes selectors itself, and 3.0 changed them.    | `Each` gives paths, and `Child` extends them.
| A save that can't keep the file's lines  | 3.0's save returns a flag, once it's done.                  | `Rewrites` asks before the save.
| A put of a key with a commented default  | 3.0 adds the key at the end of its block.                   | Fills in the `## Default` line, where it is.

- `PutSyntax` checks its text by setting it in a new document and parsing `k: text` in another. The two have to come out the same, and the value at the path has to read the same as the parsed one.

- An empty array reads back as an empty `Good` on 3.0. The read-back check also takes `Empty`, which is how 1.2 and 2.0 read one.

## Ops scripts

An ops script is one op per line, with fields split by tabs. `\t`, `\n`, `\r`, and `\\` in a field are a tab, newline, return, and backslash. Blank lines and lines starting with `#` are skipped.

| Op                                              | Prints
| :---                                            | :---
| `parse [loose\|standard\|strict]`               | `ok` or `error strict`, and the document is reloaded
| `text`, `canonical`                             | the document, one `\|`-prefixed line per line
| `rewrites`                                      | `true` or `false`, from `Rewrites`
| `diags`, `errors`                               | each diagnostic as line, severity, code, and message, or the count. A layer's line is `LAYER:LINE`.
| `unused`                                        | how many lines, then each one
| `merged`                                        | each merge's path, then its lines, one merge per line
| `commented`                                     | each commented setting's line, path, and value, and `default` for a `## Default` line
| `get TYPE PATH`                                 | status, line, and the value. Arrays print a count, then the items.
| `or TYPE PATH FALLBACK...`                      | the value or the fallback
| `has`, `count`, `line`, `names`                 | the answer for PATH
| `lines PATH`                                    | how many instances, then each one's line
| `first TYPE PATH`, `last TYPE PATH`             | `get` of the first or last instance
| `each PATH`                                     | index, line, and value, one instance per line
| `eachget TYPE PATH CHILD`                       | `get` of CHILD under each instance
| `put TYPE PATH VALUE...`                        | `ok` or `error KIND`
| `syntax PATH TEXT`, `comment PATH TEXT`         | `ok` or `error KIND`
| `remove PATH`                                   | how many instances went
| `info on\|off`, `stamp`                         | `ok` or `error KIND`, from `InfoBlock` and `StampFormat`
| `check FILE [all\|drop]`                        | the schema check, with the load's own diagnostics for `all`, and then dropped ones for `drop`
| `format`, `upgrade [FROM]`                      | the format number, or the upgrade's result, its lost and rewritten lines, and text
| `write FILE [force] [create] [backup] [mode=M]` | `ok` and the saved file, or `error KIND`. The flags are `WriteOpts`'s, with M in octal.
| `mode FILE`                                     | the file's permission bits, in octal
| `readonly`                                      | `true` or `false`, from `ReadOnly`
| `scribble FILE TEXT`                            | `ok` once TEXT is in FILE, written as another program would
| `open FILE [loose\|standard\|strict]`           | `ok`, or `error KIND` and the error's diagnostics, and the document is reloaded
| `layers SPEC...`                                | `open` for `OpenLayers`. Each SPEC is a file name, or `=` and a text layer's text.
| `upgraded`                                      | `UpgradedFrom` and `BackupName`, then the backup's text
| `files`                                         | the names in the folder

- TYPE is `int`, `float`, `bool`, `str`, `strs`, `ints`, `floats`, or `raw`.

- A float field takes decimal and exponent forms, plus `nan`, `inf`, and `-inf`. A number too big for a float is `inf`. Each language's own parser takes other spellings, which the ops leave out so both read a script the same.

- Every op is echoed first, after a `>` and a space, so an output file reads as a transcript.

- Floats print the same in every language: the shortest digits that read back exactly, positional from 1e-5 to 1e17, otherwise with an exponent. When 2 spellings of that length are equally close, the even one wins, as in Go.

- `check`, `write`, `scribble`, `open`, and `files` only work in the folder the driver is given, and refuse a name with a path in it. A case's own files are in that folder, `doc.shcl` among them. The time in a backup's name prints as `STAMP`.

## Testing

### Shared cases

- Each folder in `tests/cases` is a document, an ops script, and one `want.vN.txt` per shcl version. Go and Rust both run every case on every version and compare with the same want files, so Go and Rust can't drift apart.

- A file named `x.vK.ext` stands in for `x.ext` from version K up, for a later version whose syntax differs. Every case has one set of files today, for 3.0.

- `034-upgrade-1x-written` and `035-upgrade-2x-written` hold files the wrapper's 1.2 and 2.0 adapters wrote, kept from before those were dropped, so the upgrade is still tested on them.

- `038` through `046` hold a real file each program wrote, named by its stand-in: a committed default, a sample config, or a fixture from its tests, with project names and anything personal taken out. Each case says what `Open` makes of it. Folderkite's is the output of its own test writer, built against 2.0.

- `SHCLW_UPDATE=1 go test ./shclw/` writes the want files. A changed want file is reviewed in the diff like code.

- Every feature and every fix gets a case, or a line in an existing one.

- C runs the same cases through its driver with `cicd/c_cases.py`, op by op against the same want files. Python runs them the same way, with `--lang Python` and its driver, `source/python/shclw_drive.py`.

### Fuzzing

- `cicd/fuzz.py` makes a document and an ops script from each seed, with a generator that knows each format's syntax. Half of the generated documents are in the 1.2 and 2.0 syntax, as old programs wrote them, so the upgrade sees them. A quarter of the documents are mutated copies of the case documents instead.
	- Each driver gets the document as `doc.shcl` in its own folder. A quarter of the scripts `open` it first and end by saving over it twice, so an old one is upgraded in memory and backed up.

- Each case runs through the Go, Rust, C, and Python drivers on the same shcl version. Rust's, C's, and Python's outputs must match Go's byte for byte, apart from the time 3.0 stamps on a line it comments out. None may crash, and a put that says ok must read back the same. A float read that says `Good` must not give infinity or NaN. A `first` or `last` read must not say `Multiple`, and has to be on the first or last line `lines` gave for that path. An edit that says ok must have found one instance at every step of its path that exists, counted just before it. The text right after an `info on` that says ok ends with the block naming the version's format, holds one block where the document had no block text in a raw body, and a second `info on` doesn't change it.

- It runs for a wall-clock budget, and a failing seed is saved with its document, script, and each driver's output. If too few reads find a value, the run fails, since a generator that stopped making readable documents would pass forever. A run of under 30 cases, such as a short one on a busy box, is too few to judge that, and says so instead.

- C's driver is the sanitized build, so a leak or a bad read fails the case. The fuzz runs where the C drivers are built, in the C container or on the host. Without the pinned C headers it fuzzes Go against Rust alone.

- Go's own fuzzer runs `Run` on random documents and scripts against every version, looking for panics.

- `cicd/cicd.bash` runs format, lint, tests, the C leak checks, and both fuzzers. `--quick` leaves out the C sanitizers, analyzers, and leak checks, and anything that needs the network, and fuzzes for 10s. `--leakcheck` adds the leak checks to it, and `--no-leakcheck` drops them from any run. `--dry-run` or `--list` shows the stages a mix of flags picks.
	- 3 runs never sync, rewrite a tracked file, or prompt. Format only checks, and cargo runs `--locked`. `--merge-gate` and `--backlog-run-full` run every check of the default run. `--backlog-run-quick` runs the format check, lint, and the fast tests, and can be held to some of go, rust, python, c and cicd. `--ci` and `--gate` are other names for `--merge-gate` and `--backlog-run-quick`.
	- `--no-format`, `--no-lint`, `--no-tests`, `--no-fuzz` and `--no-sync` turn one step off. `--no-container` runs C on the host, and turns the leak checks off. C also runs on the host when docker is missing or its image won't build, and says so in one line. On the host a tool off its pin is a warning, and a check whose tool is missing, such as clang-22 for the analyzers, is a `SKIP` line. `--keep-temp` keeps the run's scratch folder.
	- The pre-push hook runs `--merge-gate` only on a push to main. A push to main that changes only `*.md` files and `project/` skips it, and so does a push to any other branch or a branch delete.
	- Each C container is named `shclwrap-<run folder>-<mode>`, with labels `dev.project=shclwrap` and `dev.run=<run folder>`.

- One run goes at a time in a tree, since runs share `cicd/artifacts/`, `cicd/cache/`, and the checkout. A second run, such as the push hook, waits for the first and says which run it waits on. `c-checks.bash` run alone takes the same lock. Runs in separate worktrees don't wait on each other. Each run still gets its own log in `cicd/artifacts/lint/`, named for the second it started, so a run started in the same second as another waits for the next second to name its log.

- The checks cover shcl's own code too, since every program built on the wrapper runs it. The sanitized builds and the fuzz drivers build it instrumented, and the analyzers and CodeQL read it. Its findings are reported but not fatal yet. Style lints, such as clang-tidy, stay on our code. Test-only and cicd-only code from elsewhere is left out, since no user runs it.

- A run removes its own scratch folder. It keeps what the next run reuses: the C container's home with CodeQL's compiled queries, the pinned headers, and the C image. It drops a cached header no pin names, an older C image no container uses, docker's dangling build cache after a rebuild, and leak reports from before the latest leak run. The newest 10 sets of fuzz failures are kept.

## Sharing the code

- Go: `go get github.com/jim-collier/shclwrap/source/go@<commit>`, then import the one version package. Copying `shclw` and the version package into a project works too.

- Rust: a git dependency on this repo with one feature on, or a copy of `source/rust/src`.

- C: a copy of `shclw.h`, `shclw_adapter.h`, `shclw.c`, `info.c`, and `shclw3.c`, plus `run.c` for ops scripts. The program builds `shcl.h`'s own implementation once, as shcl asks. It also takes the `shclw3_v2` files and shcl 2.0's `shcl.h`.

- Python: a copy of `shclw.py` and `_shclw_shcl2.py`, which is shcl 2.0's `shcl.py` kept byte for byte and checked against its pin. shcl 3.0 is a `shcl.py` beside them, or the installed one, in that order. Anything older is refused when the module loads, by `FORMAT_MAJOR`, which no binding had before 3.0.

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
	- So a 3.0 file with no Format line and an error that 2.0 reads clean, which `Open` takes as 2.x, has its backslash escapes read the 2.x way too. A program that writes its files with `StampFormat` or `InfoBlock` never has that happen.
	- A file `Open` takes as current keeps its backslashes as typed, even one 2.x wrote, when nothing in it says so.

- An upgrade lists lines of the original, not of the upgraded text, since the original is what the user wrote and the backup keeps (2026101513330012, 2026101513330013). The lists go on `Upgraded` beside `Unsure`, rather than in a call of their own like `Rewrites`, since the upgrade already has both texts and `Rewrites` is a document's.
	- A line is lost only when the upgraded file has no value where 2.0 read one. A line whose value comes through written another way is rewritten. So a 1.x line break in a bare value like `C:\new` is rewritten, not lost, since the upgrade carries it as 2.0 read it and 3.0 can write it. A program that counted that line lost sees one fewer.
	- Rewritten lines are matched as a multiset of whole lines, line ends included, rather than by a line diff. A line that only moved, as shcl's write order can do, wasn't written differently, and it stays linear in the file's size.

- `Open` upgrades an old file in memory, rather than leave it to the program to call `UpgradeFile` first. A program that only reads then never writes, and one that saves gets the backup on its first `Write` (2026101613005800).
	- It upgrades as `Upgrade` with `from` 0 does, never 2. shcl says a program calling its upgrade on every start must not pass 2, since a 3.0 file that picks up an error would then be read by 2.x rules.
	- So a 3.0 file with no Format line and an error is taken as 2.x when shcl 2.0 reads it clean, and upgraded. A program that writes its files with `StampFormat` or `InfoBlock` never has that happen.
	- One with errors on 3.0 and 2.0 both opens as current, with 3.0's errors in `Diags`, as before 3.0 was the floor. A typo in a current file then gives that file's own errors, and the guess is taken only when 2.0 backs it. A file its Format line or its 2.x block marks as 2.x is still `ErrRefused` when 2.0 has errors on it. `Upgrade` with `from` 0 does the same, so `Open`, `Upgrade`, and `UpgradeFile` agree.
	- Reads, `Lines`, `Diags`, and `Text` are the upgraded text's, rather than the original's line numbers. shcl gives no line map, the upgraded text is what the save writes and the next `Open` reads, and the original's errors are why it was upgraded.
	- `Parse` of text never upgrades. There's no file to back up, and `Upgrade` already takes text.
	- `UpgradedFrom` and `BackupName` say what happened, rather than an `Upgraded` value on the document. Its text would be the document's own, and its count of unsure lines is from `UpgradeFile`, for a program that wants it ahead of time.
	- `Rewrites` is true until the first `Write` over the file, since that one replaces every line the file had.
	- The backup goes on the first `Write` over the file `Open` read, by path or the same file by another name, rather than on any `Write`. A copy saved somewhere else replaces nothing.
	- It uses `UpgradeFile`'s backup and replace code, so a file that changed since `Open` read it is `ErrChanged`, and the backup name and `ErrBackupTaken` are the same.
	- An `Open` that's refused or ambiguous gives an empty document rather than the original read by 3.0's rules. In C, 2.0's errors are that document's diagnostics, since C's error has no list.
	- Rust's `write` takes `&mut self`, since it records the backup.

- With `from` 0, a file with no Format line that has 2.x's `#` info block is taken as 2.x, rather than as current (2026101513330010). No 3.0 writer makes that block, and Quillroute checked for it the same way.
	- A 3.0 program that saves over a 2.x file without upgrading it keeps the block, and a backslash escape it writes is then read the 2.x way. So the wrapper's own save swaps 2.x's block for the current one when the text has no Format line, as the upgrade does. A file the wrapper saves says what it is. Only programs off the wrapper keep the risk, and none is known.

- `UpgradeFile` on 3.0 does its own read, backup, and write with shcl's parts, rather than call shcl's `UpgradeFile`. shcl's reads the file again after the wrapper's 2.0 check, so an edit in between was converted with no check.

- A file that changed under `UpgradeFile` was `ErrIO`, as shcl reports it, rather than a kind of its own. Since `Write` checks every save over the file it read, it's `ErrChanged` in both (2026101513330017), so a program handles one kind for one thing.

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

- Layered loading uses shcl's own merge, rather than one of the wrapper's (2026101512024156). It's what Cobblevale calls today, and its rules are the ones a reader expects: a value, an array, or a raw block in a higher layer replaces the lower one's whole, and blocks merge by name. A merge of the wrapper's own would be one more set of rules to keep the same in 3 languages and in step with shcl. Other merging is still a non-goal, reached through `Inner()`.

- Each layer stays its own document, and edits and saves go to the last file layer. The merge is built again after each edit. Saving the merge would copy the defaults and the site file into the user's file, and a later change to either would never show. Mossrealm and Cobblevale both keep the user layer apart and save only it. Building again costs a merge of every layer per edit, which is small for config files, and it's how a removed key comes to read as the layer below has it.
	- Text layers after the last file are this run's, so a command line can be a layer without being saved, as Mossrealm's 4th layer is.
	- The layers below stay as they were read, so saving only what differs from them (2026101513330030) can build on this.

- A missing layer file is skipped. Any other error stops the load, as `Open` does, rather than leave that layer out. Leaving out a broken site file would hide it, and leaving out a user file that's too new would let the next save write over it.

- A diagnostic names its layer by its place in the list, `Layer`, rather than by a name. The program already has the list, C gets no string to free, and a document that isn't layered gives the same diagnostics as before.

- `Unused` and `Merged` are calls beside `Diags`, rather than fields filled in at load (2026101513330018, 2026101513330009). A field would cost every load the work, `Merged`'s two parses most of all, and a call matches `Diags`.
	- The lines are a list of `Spot`s rather than plain numbers, as the upgrade's are, since a layered document's line needs its layer to mean anything.

- `Unused` is every error's line, rather than only the lines shcl dropped from a canonical save (its `LostCount`). A line kept as written but read as nothing is still a line the program didn't get. Wovenshell builds its list from the errors too, and leaves out line 0, an error about the whole file.

- `Merged` finds a folded copy by its line: it's on no node, and has no error. Each such line that starts with a name gets that name made unique, and the text is parsed again, so the copy stands alone, at the line it came from, under the same parent. The kept copy is then the one before it, under that parent, with the same name and value. shcl would be the better place for this, since its parser sees each fold happen, but it has no call for it, and H002 leaves the next-to-each-other case out on purpose. A call in shcl can replace this later with the same results.
	- Writing the parser's rules again in each wrapper was the other choice. It would have to know raw blocks, lists, and dotted paths to tell a copy from a line that only looks like one. The second parse gets that from shcl.
	- A raw block's body line that looks like a copy is left alone. Its renamed name is still raw text, so no node shows up at its line.

- A newer file opens read-only rather than failing with `ErrTooNew` (2026101513330014). A program that only reads gets its settings, and one that writes finds out at `Write`, with the same `ErrTooNew` it got from `Open` before. Returning the document beside an `ErrTooNew` from `Open` was the other choice. It would keep every caller that stops on any error as it was, but `OpenLayers` would then need its own rule to skip that one error.
	- `Write` refuses with `ErrTooNew` and not a new kind, since that's the reason, and programs already handle it.

- `ErrChanged` is a kind of its own, where the upgrade's backup used `ErrIO` for the same thing before (2026101513330017). A program can act on it, by reading again and redoing its edit, which it can't on a disk error. `UpgradeFile` and the first `Write` over an upgraded file give it too now.
	- The check is on by default, since a lost save is worse than a refused one. `Force` turns it off for one `Write`.
	- It compares the text `Open` read, rather than a hash or size and mtime. It's exact, it was already there for the backup, and the cost is a copy of a config file. An mtime can stay the same across 2 saves in one tick, or change with nothing new.
	- `WriteOpts` is a struct so the create-only Write (2026101513330015) and a file mode (2026101513330016) can be fields on it later.

- Create-only is a link of a synced temp file, rather than an exclusive create of the name (2026101513330015). The exclusive create alone has the one-winner rule, but a reader or a crash can catch the file half written. A filesystem with no hard links falls back to the exclusive create, rather than refusing, since a save that fails on a USB stick is worse than one that's not atomic there.
	- A taken name is `ErrExists`, a kind of its own, since a program acts on it, by reading the file that's there.

- `Mode` is exact, set by chmod past the umask, rather than masked by it (2026101513330016). A program taht asks for 0600 means 0600, and a umask can only take bits away anyway, so masking would change nothing for 0600 and surprise anyone who asked for 0644 under a umask of 077.
	- It applies only to a file the `Write` creates, as the mode of `open(2)` does. An existing file keeps its mode, which shcl's atomic write already did, so a program never changes a file's mode by saving it.

- A save that would lose lines copies the file aside only when asked, with `WriteOpts.Backup`, rather than always (2026101513330019). The refusal stays the default, since a program that never looks at `BackupName` would leave copies piling up beside the file with no word to the user. Cobblevale asks.
	- It's a flag rather than a save of the lines somewhere of the wrapper's choosing, since the upgrade's backup already has a name, a format tag, and a taken-name error programs handle.
	- The copy is of the file `Open` read, so a `Write` elsewhere stays refused. Over another path the file `Open` read still has the lines, but a `Parse` document has no file at all, and one rule is easier to explain.

- A put fills in a commented line only when it has the `## Default` note, not any commented `key: value` (2026101513330020). The note says a program wrote the line as a default it can take back. A commented line without it can be the user's own, and filling it in would lose it.
	- The note comes off, since the value set is no longer the default.
	- The fill-in parses the text again, since shcl has no call to turn a comment line into a field. That costs a load for each fill-in, and gives keys earlier puts added their real lines.

- `Commented` finds a line's path by parsing the text again with the line uncommented, rather than by walking indents (2026101513330021). shcl's own parse knows dotted names, quoting, raw bodies, and repeated blocks, which an indent walker like Wovenshell's has to get right one by one.

- There's no `OpenLayersWith` yet. A program that wants strict layers checks `Errors()`, which counts every layer.

- C's `shclw_drop_declared` takes the document and drops hints from its own diagnostics, rather than taking a list as Go and Rust do. shcl's C binding has only that form, and the match is on hint text that shcl builds inside itself.

- The build gets `shcl.h` by commit, checked by sha256, into a cache the repo ignores, rather than keeping a copy of each version in the repo. That's how Go and Rust get theirs too, through their package managers. The pins are in `source/c/shcl-pins.env`.

## Open questions

- When shcl 3.0's first beta is cut, the Go module path is still `.../v2`. Go won't take a `v3.0.0-beta1` tag on a `/v2` path, so the pin stays a commit until shcl's module path changes.

- shcl 4's API is a draft. Its adapter waits for 4 to exist in each language, which is planned Rust first, then C, then Go and Python.

## Copyright and license

> Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]<br />
> Licensed under the [MIT License](https://mit-license.org/)<br />
> SPDX-License-Identifier: `MIT`
