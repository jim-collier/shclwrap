// Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
// SPDX-License-Identifier: MIT

//! Runs every shared case against every shcl version and compares with the
//! case's want.vN.txt, the same files the Go tests read.

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

use shclw::{run, Lib, RunOptions};

mod common;
use common::Scratch;

fn cases_dir() -> PathBuf {
	Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/cases")
}

fn scratch(root: &Path, case: &str, n: u32) -> PathBuf {
	let dir = root.join(format!("{case}-v{n}"));
	fs::create_dir_all(&dir).unwrap();
	dir
}

/// Copies a case into `dir` as version `n` sees it. A file named x.vK.ext
/// stands in for x.ext from version K up, until a higher K takes over.
fn materialize(from: &Path, dir: &Path, n: u32) -> bool {
	let mut best: BTreeMap<String, (u32, String)> = BTreeMap::new();
	for e in fs::read_dir(from).unwrap() {
		let name = e.unwrap().file_name().to_string_lossy().into_owned();
		let (base, k) = split_version(&name);
		if base.starts_with("want.") || k > n {
			continue;
		}
		if best.get(&base).is_some_and(|(prev, _)| *prev >= k) {
			continue;
		}
		best.insert(base, (k, name));
	}
	for (base, (_, src)) in &best {
		fs::copy(from.join(src), dir.join(base)).unwrap();
	}
	best.contains_key("doc.shcl")
}

/// "doc.v3.shcl" is ("doc.shcl", 3); a name with no version is version 0.
fn split_version(name: &str) -> (String, u32) {
	let parts: Vec<&str> = name.split('.').collect();
	if parts.len() >= 3 {
		let mid = parts[parts.len() - 2];
		if let Some(k) = mid.strip_prefix('v').and_then(|d| d.parse::<u32>().ok()) {
			let mut base = parts[..parts.len() - 2].join(".");
			base.push('.');
			base.push_str(parts[parts.len() - 1]);
			return (base, k);
		}
	}
	(name.to_owned(), 0)
}

fn run_all<L: Lib>(root: &Path, n: u32, failures: &mut Vec<String>, ran: &mut usize) {
	let mut cases: Vec<_> = fs::read_dir(cases_dir())
		.unwrap()
		.map(|e| e.unwrap().path())
		.filter(|p| p.is_dir())
		.collect();
	cases.sort();
	for case in cases {
		let name = case.file_name().unwrap().to_string_lossy().into_owned();
		let dir = scratch(root, &name, n);
		if !materialize(&case, &dir, n) {
			continue;
		}
		*ran += 1;
		let doc = fs::read_to_string(dir.join("doc.shcl")).unwrap();
		let ops = fs::read_to_string(dir.join("ops.txt")).unwrap();
		let got = run::<L>(&doc, &ops, &RunOptions { dir: Some(dir.clone()) });
		let want_path = case.join(format!("want.v{n}.txt"));
		match fs::read_to_string(&want_path) {
			Ok(want) if want == got => {}
			Ok(_) => failures.push(format!(
				"{name}/v{n}: output differs from {}:\n--- got\n{got}",
				want_path.display()
			)),
			Err(e) => failures.push(format!("{name}/v{n}: no want file ({e}); the Go tests write them")),
		}
	}
}

#[test]
fn cases() {
	let root = Scratch::new("cases");
	let mut failures = Vec::new();
	let mut ran = 0;
	run_all::<shclw::v3::Version>(root.path(), 3, &mut failures, &mut ran);
	assert!(ran > 0, "no cases ran");
	assert!(
		failures.is_empty(),
		"{} of {ran} failed:\n{}",
		failures.len(),
		failures.join("\n")
	);
}
