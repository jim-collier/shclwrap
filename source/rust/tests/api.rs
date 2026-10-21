// Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
// SPDX-License-Identifier: MIT

//! The calls the ops scripts can't reach: read-back checks and opening files.

use std::fs;
use std::path::PathBuf;

use shclw::{verify_put, ErrKind, Lib, ShclDoc, Status, Value};

mod common;
use common::Scratch;

fn check_verify<L: Lib>(n: u32) {
	let d = L::parse("a: 1\n");
	assert!(verify_put(&d, "a", &Value::Int(1)).is_ok(), "v{n}: matching value");
	let e = verify_put(&d, "a", &Value::Int(2)).expect_err("different value");
	assert_eq!(e.kind, ErrKind::NoReadBack, "v{n}: different value");
	let e = verify_put(&d, "nope", &Value::Strs(Vec::new())).expect_err("missing array");
	assert_eq!(e.kind, ErrKind::NoReadBack, "v{n}: missing array");
}

#[test]
fn verify() {
	check_verify::<shclw::v3::Version>(3);
}

/// An f32 has to go in as its own shortest digits. Widened to f64 first,
/// 0.9 was written as 0.8999999761581421.
// The read-back has to be exact.
#[allow(clippy::float_cmp)]
fn check_f32<L: Lib>(n: u32) {
	let texts = [(0.9_f32, "f: 0.9\n"), (0.1, "f: 0.1\n"), (-2.5, "f: -2.5\n")];
	for f in [0.9_f32, 0.1, -2.5, 1e-10, f32::MAX, 16_777_216.0, f32::from_bits(1)] {
		let mut d = L::parse("");
		if let Err(e) = d.put("f", f) {
			panic!("v{n} {f}: {e}");
		}
		let want: f64 = f.to_string().parse().unwrap();
		let g = d.get_float("f");
		assert_eq!((g.status, g.value), (Status::Good, want), "v{n} {f}: read back");
		if let Some((_, text)) = texts.iter().find(|(x, _)| *x == f) {
			assert_eq!(d.text(), *text, "v{n} {f}: text");
		}
	}
	// An exact tie goes to even, as Go's strconv has it, but at an f32 power
	// of 2 Go rounds up.
	for (bits, want) in [
		(0x3b20_0000_u32, "0.0024414062"),
		(0xbb20_0000, "-0.0024414062"),
		(0x3980_0000, "0.00024414063"),
	] {
		let f = f32::from_bits(bits);
		let mut d = L::parse("");
		if let Err(e) = d.put("f", f) {
			panic!("v{n} {f}: {e}");
		}
		let g = d.get_float("f");
		assert_eq!(
			(g.status, g.value),
			(Status::Good, want.parse().unwrap()),
			"v{n} {f}: tie"
		);
	}
	for f in [f32::NAN, f32::INFINITY] {
		let mut d = L::parse("");
		let e = d.put("f", f).expect_err("non-finite put");
		assert_eq!(e.kind, ErrKind::BadValue, "v{n} {f}");
		assert!(!d.has("f"), "v{n} {f}: the put changed the document");
	}
}

#[test]
fn put_f32() {
	check_f32::<shclw::v3::Version>(3);
}

#[test]
fn open() {
	let scratch = Scratch::new("api");
	let dir = scratch.path();
	let newer = dir.join("newer.shcl");
	fs::write(&newer, "a: 1\n##    Format   9\n").unwrap();
	let binary = dir.join("binary.shcl");
	fs::write(&binary, b"a: \xff\xfe\n").unwrap();
	let missing = dir.join("missing.shcl");
	let s = |p: &PathBuf| p.to_string_lossy().into_owned();
	let kind = |r: Result<(), shclw::Error>| r.err().map(|e| e.kind);
	// A newer file opens read-only, so it's the save that refuses.
	let open_save = |p: &str| shclw::v3::open(p).and_then(|mut d| if d.read_only() { d.write(p) } else { Ok(()) });
	for (path, want) in [
		(&newer, ErrKind::TooNew),
		(&binary, ErrKind::Io),
		(&missing, ErrKind::NotFound),
	] {
		assert_eq!(kind(open_save(&s(path))), Some(want), "v3 {}", path.display());
	}

	// A Format line in a raw body is the body's. A real one after it still
	// counts.
	let raw = "a: 1\ntmpl:\n\t~~~\n##    Format   9\n\t~~~\n";
	let in_raw = dir.join("raw.shcl");
	fs::write(&in_raw, raw).unwrap();
	let raw_then_real = dir.join("rawreal.shcl");
	fs::write(&raw_then_real, format!("{raw}##    Format   9\n")).unwrap();
	let open = |p: &PathBuf| kind(open_save(&s(p)));
	assert_eq!(open(&in_raw), None, "v3 raw.shcl");
	assert_eq!(open(&raw_then_real), Some(ErrKind::TooNew), "v3 rawreal.shcl");
}

/// An edit made through `inner_mut`, with calls the wrapper doesn't have,
/// has to show up in the wrapper's own reads and text.
fn check_inner<D: ShclDoc>(n: u32, d: &D, paths: &[String]) {
	assert!(paths.iter().any(|p| p == "a"), "v{n}: inner().paths() = {paths:?}");
	assert_eq!(d.get_str("a").status, Status::Empty, "v{n}: after set_empty");
	assert_eq!(d.str("b", ""), "set inside", "v{n}: after set_string");
	assert!(d.text().contains("set inside"), "v{n}: text() misses the inner edit");
}

#[test]
fn inner() {
	let mut d3 = shclw::v3::parse("a: 1\n");
	assert_eq!(
		d3.inner_mut().set_empty("a"),
		shclw::v3::shcl::SetStatus::Ok,
		"v3 set_empty"
	);
	assert_eq!(
		d3.inner_mut().set_string("b", "set inside"),
		shclw::v3::shcl::SetStatus::Ok,
		"v3 set_string"
	);
	check_inner(3, &d3, &d3.inner().paths());
}

/// A 2.x file the old shcl reported errors on is refused, and `upgrade_file`
/// leaves it as it was, with no backup. A clean one still goes.
#[test]
fn upgrade_refuses_old_errors() {
	let scratch = Scratch::new("upgrade");
	let dir = scratch.path().join("c");
	for (text, line) in [("a: 1\nb: ```\nraw line\nc: 2\n", 2), ("x: [ab]\ny: 1\n", 1)] {
		let e = shclw::v3::upgrade(text, 2).expect_err("upgrade");
		assert_eq!(e.kind, ErrKind::Refused, "upgrade {text:?}");
		assert_eq!(e.diags.first().map(|d| d.line), Some(line), "upgrade {text:?}");
		let _ = fs::remove_dir_all(&dir);
		fs::create_dir_all(&dir).unwrap();
		let path = dir.join("c.shcl");
		fs::write(&path, text).unwrap();
		let e = shclw::v3::upgrade_file(&path.to_string_lossy(), 2).expect_err("upgrade_file");
		assert_eq!(e.kind, ErrKind::Refused, "upgrade_file {text:?}");
		assert_eq!(
			fs::read_to_string(&path).unwrap(),
			text,
			"upgrade_file changed the file"
		);
		assert_eq!(
			fs::read_dir(&dir).unwrap().count(),
			1,
			"upgrade_file {text:?}: files beside it"
		);
	}
	let u = shclw::v3::upgrade("tags: a, b\n", 2).expect("clean 2.x file");
	assert!(u.changed, "clean 2.x file: {u:?}");
}

/// A new document has no lines of its own to keep, so it never rewrites. A
/// load at any strictness counts the same as `parse`.
fn check_rewrites_new<L: Lib>(n: u32, mut fresh: L::Doc) {
	fresh.put("a.b", 1_i64).expect("put");
	assert!(!fresh.rewrites(), "v{n}: new document says it rewrites");
	let (loose, _) = L::parse_keep("a:   1\n", shclw::Strictness::Loose);
	assert_eq!(
		loose.rewrites(),
		n < 3,
		"v{n}: loose load of a text that isn't canonical"
	);
}

#[test]
fn rewrites_new() {
	check_rewrites_new::<shclw::v3::Version>(3, shclw::v3::new());
}

/// `first` and `last` pick by line, so the fallback reads and an edit take
/// that instance alone. An instance a put made has no line and sorts last.
fn check_first_last<L: Lib>(n: u32) {
	let mut d = L::parse("p: 1\ntv: a\n\tf: 1\ntv: b\n\tf: 2\ntv: a\n\tf: 3\np: 2\n");
	assert_eq!(d.int(&d.last("p"), 9), 2, "v{n}: int of last p");
	assert_eq!(d.str(&d.first("tv.f"), ""), "1", "v{n}: str of first tv.f");
	assert_eq!(d.int(&d.last("tv.f"), 9), 3, "v{n}: int of last tv.f");
	assert_eq!(d.int("p", 9), 9, "v{n}: plain int of p");
	let last = d.last("p");
	d.put(&last, 5).unwrap_or_else(|e| panic!("v{n}: put on last p: {e}"));
	assert_eq!(d.get_int(&d.first("p")).value, 1, "v{n}: first p after the put");
	assert_eq!(d.int(&d.last("p"), 9), 5, "v{n}: last p after the put");
	assert_eq!(d.lines("p"), [1, 8], "v{n}: lines of p after the put");
	let tv = d.each("tv");
	let (a, b) = (d.child(&tv[0].path, "g"), d.child(&tv[1].path, "g"));
	d.put(&b, 2).unwrap_or_else(|e| panic!("v{n}: {e}"));
	d.put(&a, 1).unwrap_or_else(|e| panic!("v{n}: {e}"));
	assert_eq!(d.int(&d.last("tv.g"), 9), 2, "v{n}: last of two put instances");
	assert_eq!(d.lines("tv.g"), [0, 0], "v{n}: lines of two put instances");
	assert_eq!(d.last("nope"), "nope", "v{n}: last of a missing path");
}

#[test]
fn first_last() {
	check_first_last::<shclw::v3::Version>(3);
}

/// tv and u.v repeat at an inner step, n at the last one. Only one tv has a
/// g, and only one v has a w.
const SEVERAL: &str = "tv: one\n\tg: 1\ntv: two\nn: 1\nn: 2\nu:\n\tv: 1\n\tv: 2\n\t\tw: 1\n";

type Edit<D> = fn(&mut D, &str) -> Result<(), shclw::Error>;

/// Every edit on `ShclDoc`. Go's `TestDocMethodsSorted` fails on a new method
/// until it's sorted, and the trait has the same set.
fn edits<D: ShclDoc>() -> [(&'static str, Edit<D>); 3] {
	[
		("put", |d, p| d.put(p, 5)),
		("put_syntax", |d, p| d.put_syntax(p, "6")),
		("comment", |d, p| d.comment(p, "c")),
	]
}

/// The lines of a and b between their common start and end.
fn hunk(a: &str, b: &str) -> (usize, usize) {
	let (x, y): (Vec<_>, Vec<_>) = (a.split('\n').collect(), b.split('\n').collect());
	let start = x.iter().zip(&y).take_while(|(p, q)| p == q).count();
	let end = x[start..]
		.iter()
		.rev()
		.zip(y[start..].iter().rev())
		.take_while(|(p, q)| p == q)
		.count();
	(x.len() - start - end, y.len() - start - end)
}

/// Every edit refuses a path that reaches several instances at any step, as
/// 3.0 does, and changes nothing. A path from `each`, `first`, or `last`
/// names one at every step, and an edit on it changes one place.
fn check_several<L: Lib>(n: u32) {
	for (name, edit) in edits::<L::Doc>() {
		for p in ["n", "n.x", "tv", "tv.g", "tv.h", "tv.g.h", "u.v", "u.v.w", "u.v.w.x"] {
			let mut d = L::parse(SEVERAL);
			let was = d.text();
			let e = edit(&mut d, p).expect_err(&format!("v{n} {name} {p}"));
			assert_eq!(e.kind, ErrKind::BadPath, "v{n} {name} {p}");
			assert_eq!(d.text(), was, "v{n} {name} {p}: changed");
		}
		let d = L::parse(SEVERAL);
		let mut paths: Vec<String> = d.each("tv").iter().map(|t| d.child(&t.path, "h")).collect();
		paths.extend(d.each("u.v").iter().map(|t| d.child(&t.path, "w")));
		paths.extend(d.each("n").into_iter().map(|t| t.path));
		paths.extend([d.first("tv.g"), d.last("u.v.w"), d.last("n")]);
		for p in &paths {
			let mut d = L::parse(SEVERAL);
			let was = d.text();
			edit(&mut d, p).unwrap_or_else(|e| panic!("v{n} {name} {p}: {e}"));
			// A put adds or changes a line, and a comment on a new name adds 2.
			let (gone, added) = hunk(&was, &d.text());
			assert!(gone <= 1 && added <= 2, "v{n} {name} {p}: changed more than one place");
		}
	}
	for p in ["n", "tv.g", "u.v", "u.v.w"] {
		let mut d = L::parse(SEVERAL);
		let want = d.count(p);
		assert_eq!(d.remove(p), want, "v{n}: remove {p}");
		assert!(!d.has(p), "v{n}: {p} left after remove");
	}
	let d = L::parse("a:\n\tx:\n\t\tb: 1\n\tz:\n\t\tb: 2\n\ty:\n\t\tc: 3\n\tx: again\n\t\tb: 4\n");
	let nodes = d.each("a.*.b");
	let lines: Vec<usize> = nodes.iter().map(|t| t.line).collect();
	let vals: Vec<i64> = nodes.iter().map(|t| d.get_int(&t.path).value).collect();
	assert_eq!((lines, vals), (vec![3, 5, 9], vec![1, 2, 4]), "v{n}: each of a.*.b");
	assert_eq!(d.int(&d.last("a.*.b"), 0), 4, "v{n}: last of a.*.b");
}

#[test]
fn several() {
	check_several::<shclw::v3::Version>(3);
}

/// Every info block shcl's `GEN_BANNER` has been, in order, and 2.x's. The
/// betas linked tags that were never cut. `{n}` is the format.
const OLD_BLOCKS: [&str; 6] = [
	"#\n# This config file format is SHCL.\n# \"Simple Hierarchical Config Language\"\n#    Home     https://github.com/jim-collier/shcl\n#    Syntax   https://github.com/jim-collier/shcl/blob/main/project/spec.md\n#    Legal    SHCL is Copyright © 2026 Jim Collier. License: MIT. No warranty.\n#\n",
	"##\n## This config file format is SHCL.\n## \"Simple Hierarchical Config Language\"\n##    Home     https://github.com/jim-collier/shcl\n##    Syntax   https://github.com/jim-collier/shcl/blob/main/project/spec.md\n##    Legal    SHCL is Copyright © 2026 Jim Collier. License: MIT. No warranty.\n##\n",
	"##\n## This config file format is SHCL.\n## \"Simple Hierarchical Config Language\"\n##    Home     https://github.com/jim-collier/shcl\n##    Syntax   https://github.com/jim-collier/shcl/blob/main/project/spec.md\n##    Legal    SHCL is Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]. License: MIT. No warranty.\n##\n",
	"##\n## This config file format is SHCL.\n## \"Simple Hierarchical Config Language\"\n##    Format   {n}\n##    Home     https://github.com/jim-collier/shcl\n##    Syntax   https://github.com/jim-collier/shcl/blob/main/project/spec.md\n##    Legal    SHCL is Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]. License: MIT. No warranty.\n##\n",
	"##\n## This config file format is SHCL.\n## \"Simple Hierarchical Config Language\"\n##    Format   {n}\n##    Home     https://github.com/jim-collier/shcl\n##    Syntax   https://github.com/jim-collier/shcl/blob/v3.0.0/project/spec.md\n##    Legal    SHCL is Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]. License: MIT. No warranty.\n##\n",
	"##\n## This config file format is SHCL.\n## \"Simple Hierarchical Config Language\"\n##    Format   {n}\n##    Home     https://github.com/yottacore/shcl\n##    Syntax   https://github.com/yottacore/shcl/blob/v3.0.0/project/spec.md\n##    Legal    SHCL is Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]. License: MIT. No warranty.\n##\n",
];

// Off since 3.0 is the floor: info_text was the 1.2 and 2.0 block, and is gone.
// #[test]
// fn info_text_is_gen_banner() {
// 	assert_eq!(
// 		shclw::info_text(shclw::v3::FORMAT_MAJOR, "v3.0.0-beta1"),
// 		shclw::v3::shcl::GEN_BANNER
// 	);
// }

/// Each old spelling, and the current one, above the first field, between
/// two, and at the end, becomes one block at the end, and every version
/// leaves the same text.
fn check_old_spellings<L: Lib>(n: u32) {
	let mut blocks: Vec<String> = OLD_BLOCKS.iter().map(|b| b.replace("{n}", &n.to_string())).collect();
	blocks.push(shclw::v3::shcl::GEN_BANNER.to_owned());
	for (i, old) in blocks.iter().enumerate() {
		for text in [
			format!("{old}a: 1\nb: 2\n"),
			format!("a: 1\n{old}b: 2\n"),
			format!("a: 1\nb: 2\n\n{old}"),
		] {
			let mut d = L::parse(&text);
			d.info_block(true)
				.unwrap_or_else(|e| panic!("v{n} spelling {i} in {text:?}: {e}"));
			assert_eq!(
				d.text(),
				format!("a: 1\nb: 2\n\n{}", shclw::v3::shcl::GEN_BANNER),
				"v{n} spelling {i} in {text:?}"
			);
		}
	}
}

#[test]
fn info_old_spellings() {
	check_old_spellings::<shclw::v3::Version>(3);
}

/// An edit after `info_block` that names a newer format leaves `text`
/// without the block, and `write` refuses, writing nothing.
fn check_info_write_refused<L: Lib>(n: u32, dir: &std::path::Path) {
	let mut d = L::parse("a: 1\n");
	d.info_block(true).unwrap();
	d.comment("a", "##    Format   9").unwrap();
	assert!(!d.text().contains("SHCL"), "v{n}: text is {}", d.text());
	let path = dir.join(format!("v{n}.shcl"));
	let e = d.write(path.to_str().unwrap()).expect_err("write");
	assert_eq!(e.kind, ErrKind::TooNew, "v{n}");
	assert!(!path.exists(), "v{n}: write left a file");
}

#[test]
fn info_write_refused() {
	let scratch = Scratch::new("info");
	check_info_write_refused::<shclw::v3::Version>(3, scratch.path());
}
