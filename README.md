<!-- markdownlint-disable MD007 -- Unordered list indentation -->
<!-- markdownlint-disable MD010 -- No hard tabs -->
<!-- markdownlint-disable MD033 -- No inline html -->
<!-- markdownlint-disable MD055 -- Table pipe style [Expected: leading_and_trailing; Actual: leading_only; Missing trailing pipe] -->
<!-- markdownlint-disable MD041 -- First line in a file should be a top-level heading -->
<div align="center">

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
![Lifecycle: Alpha](https://img.shields.io/badge/Lifecycle-Alpha-orange)
![Support](https://img.shields.io/badge/Support-Maintained-brightgreen)
![Go](https://img.shields.io/badge/Go-00ADD8?logo=go&logoColor=white)
[![made-with-rust](https://img.shields.io/badge/Made%20with-Rust-1f425f.svg)](https://www.rust-lang.org/)
![C](https://img.shields.io/badge/C-A8B9CC?logo=c&logoColor=white)

</div>

<!-- TOC ignore:true -->
# shclwrap

One small, steady API over every version of [shcl](https://github.com/yottacore/shcl), in each language that uses it.

<!-- TOC ignore:true -->
## Table of contents

<!-- TOC -->

- [Why](#why)
- [Features](#features)
- [Using it](#using-it)
	- [Go](#go)
	- [Rust](#rust)
	- [C](#c)
- [Building from source](#building-from-source)
- [Support](#support)
- [Copyright and license](#copyright-and-license)

<!-- /TOC -->

## Why

shcl is moving fast. 1.2, 2.0, and 3.0 came out within a few months of each other, 3.0 changes the file format a little, and 4 is set to change the API a lot. A program that calls shcl directly has to follow every one of those changes, and each program ends up with its own fixes for the same quirks.

This wraps the basics, loading, reading, walking, writing, saving, and upgrading, in one API that stays put. A program picks its shcl version in one place. When shcl changes, the wrapper changes, once, with tests, and every program using it gets the fix.

If your program uses one shcl version and doesn't plan to move, you don't need this. Call shcl directly. And if it leans on shcl's deeper features, like line-keeping merges or schema generation, the wrapper won't cover those. It only gets out of the way, with `Inner()`.

## Features

- The same calls in Go, Rust, and C, over shcl 3.0. That's the floor, and files written on 1.2 and 2.0 come forward through the upgrade.

- Reads with a fallback, `doc.Int("server.port", 8080)`, or with a status when the reason matters.

- Walking repeated entries without writing selector syntax, which changed between versions.

- Every write is read back. A value shcl stored differently than it was given comes back as an error.

- A file written for a newer format is refused with a clear error.

- Old files are upgraded as they're opened, in memory. The first save keeps the original beside it, and a program that only reads never writes.

- A text script of edits and reads, which is also what the tests run in every language, so the languages can't drift apart.

## Using it

### Go

~~~bash
go get github.com/jim-collier/shclwrap/source/go@dev
~~~

~~~go
import shclw "github.com/jim-collier/shclwrap/source/go/shclw3"

doc, err := shclw.Open("app.shcl")
if err != nil && shclw.KindOf(err) != shclw.ErrNotFound {
	return err
}
port := doc.Int("server.port", 8080)
for _, t := range doc.Each("table") {
	fmt.Println(doc.Str(doc.Child(t.Path, "name"), ""))
}
if err := doc.Put("server.port", 9090); err != nil {
	return err
}
return doc.Write("app.shcl")
~~~

`shclw3` matches shcl 3.0. A program still on 1.2 or 2.0 moves to 3.0 along with the wrapper, and `Open` brings its old files forward. It upgrades one in memory and `UpgradedFrom` says so. The first `Write` over the file keeps the original beside it, and `BackupName` says where. `UpgradeFile` converts a file ahead of time instead.

A file with no Format line is taken for an old one when it has 2.x's info block, or when it has errors that shcl 2.0 doesn't. A save swaps that old block for the current one, which names the format. Saving after `StampFormat()` or `InfoBlock(true)` makes any file name its format, so it never is.

### Rust

~~~toml
[dependencies]
shclw = { git = "https://github.com/jim-collier/shclwrap", branch = "dev", features = ["shcl3"] }
~~~

~~~rust
use shclw::ShclDoc;

let mut doc = shclw::v3::open("app.shcl")?;
let port = doc.int("server.port", 8080);
for t in doc.each("table") {
	println!("{}", doc.str(&doc.child(&t.path, "name"), ""));
}
doc.put("server.port", 9090)?;
doc.write("app.shcl")?;
~~~

The feature `shcl3` turns on the module `v3`, and brings shcl 2.0 along for the upgrade's check.

### C

Copy `shclw.h`, `shclw_adapter.h`, `shclw.c`, `info.c`, `shclw3.c`, and the 4 `shclw3_v2*` files from `source/c`, and build `shcl.h`'s own implementation in one file of the program's, as shcl asks. Below that's `shcl_impl.c`, which has `#define SHCL_IMPLEMENTATION` above `#include "shcl.h"`.

The `shcl.h` has to be 3.0's. With 1.2's or 2.0's the build stops with a message saying so. There's no version macro to set.

The wrapper also builds shcl 2.0 in, under other names, to check a 2.x file before upgrading it. The 2 `shclw3_v2` .c files are built with 2.0's `shcl.h` on the include path in place of 3.0's:

~~~bash
cc -I shcl-3.0 -c shclw.c info.c shclw3.c shcl_impl.c app.c
cc -I shcl-2.0 -c shclw3_v2.c shclw3_v2_impl.c
cc *.o -lm -o app
~~~

~~~c
shclw_doc *doc;
shclw_error err;
shclw_kind k = shclw_open("app.shcl", &doc, &err);
if (k != SHCLW_OK && k != SHCLW_ERR_NOT_FOUND) {
	fprintf(stderr, "%s\n", err.msg);
	shclw_free(doc);
	return 1;
}
int64_t port = shclw_int(doc, "server.port", 8080);
size_t n;
shclw_node *tables = shclw_each(doc, "table", &n);
for (size_t i = 0; i < n; i++) {
	char *path = shclw_child(doc, tables[i].path.p, "name", 4, NULL);
	char *name = shclw_str(doc, path, "", 0, NULL);
	puts(name);
	free(name);
	free(path);
}
free(tables);
k = shclw_put_int(doc, "server.port", 9090, &err);
if (k == SHCLW_OK)
	k = shclw_write(doc, "app.shcl", &err);
shclw_free(doc);
~~~

What a call hands back is the caller's to `free()`, and a document goes to `shclw_free`. Strings come back with their length and a NUL after it. A value can hold a NUL, so go by the length. `shclw.h` has the rest.

The full list of calls, and what the wrapper does differently from each shcl version, is in [design.md](project/design.md).

## Building from source

Go 1.21 or newer, Rust 1.88 or newer, and a C11 compiler. The C lint also wants gcc-15, clang-format, and clang-tidy. The pipeline pins Rust 1.96 in `source/rust/rust-toolchain.toml`.

~~~bash
cicd/cicd.bash           # everything: format, lint, tests, leak checks, and fuzzing
cicd/cicd.bash --ci      # format check, lint, and tests, as the pre-push hook runs it
cicd/cicd.bash --quick   # the fast checks only, about a minute, no network
~~~

## Support

If it saves you some time, you can say thanks through [GitHub Sponsors](https://github.com/sponsors/jim-collier) or [Ko-fi](https://ko-fi.com/jimcollier).

## Copyright and license

> Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]<br />
> Licensed under the [MIT License](https://mit-license.org/)<br />
> SPDX-License-Identifier: `MIT`<br />
> No warranty.<br />
> shclwrap™ is a [trademark](trademark.md) of Jim Collier.
