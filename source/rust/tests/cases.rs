// Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
// SPDX-License-Identifier: MIT

//! Runs every shared case against every shcl version and compares with the
//! case's want.vN.txt, the same files the Go tests read. It's its own harness
//! (`harness = false`), so each case prints a PASS, FAIL or SKIP line by the
//! test ID on the first line of its ops.txt (2026102507142902). A plain
//! argument holds the run to cases whose name has it, and `--list` lists them.

use std::collections::BTreeMap;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

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

/// The test ID on the first line of the case's ops.txt, kept with the case.
fn case_id(case: &Path) -> Option<String> {
	let ops = fs::read_to_string(case.join("ops.txt")).ok()?;
	let id = ops.lines().next()?.strip_prefix("# Test ID: ")?.trim_end_matches('\r');
	let crockford = |b: u8| b"0123456789abcdefghjkmnpqrstvwxyz".contains(&b);
	(id.len() == 5 && id.bytes().all(crockford)).then(|| id.to_owned())
}

/// The reason on a case's `# Not on Windows:` line, for a case whose output
/// differs there by design.
fn not_on_windows(case: &Path) -> Option<String> {
	let ops = fs::read_to_string(case.join("ops.txt")).ok()?;
	ops.lines()
		.find_map(|l| l.strip_prefix("# Not on Windows: "))
		.map(|why| why.trim_end_matches('\r').to_owned())
}

#[derive(Default)]
struct Tally {
	ran: usize,
	failed: usize,
}

impl Tally {
	fn fail(&mut self, line: Option<String>, why: &str) {
		self.failed += 1;
		if let Some(line) = line {
			println!("FAIL {line}");
		}
		let _ = std::io::stdout().flush(); // keep the FAIL line ahead of its reason
		eprintln!("  {why}");
	}
}

fn run_all<L: Lib>(root: &Path, n: u32, filter: Option<&str>, tally: &mut Tally) {
	let mut cases: Vec<_> = fs::read_dir(cases_dir())
		.unwrap()
		.map(|e| e.unwrap().path())
		.filter(|p| p.is_dir())
		.collect();
	cases.sort();
	for case in cases {
		let name = case.file_name().unwrap().to_string_lossy().into_owned();
		if filter.is_some_and(|f| !name.contains(f)) {
			continue;
		}
		let Some(id) = case_id(&case) else {
			tally.fail(None, &format!("{name}: no '# Test ID:' first line in ops.txt"));
			continue;
		};
		let line = format!("{id} Rust case {name}");
		let dir = scratch(root, &name, n);
		if !materialize(&case, &dir, n) {
			println!("SKIP {line}, no doc for shcl v{n}");
			continue;
		}
		if cfg!(windows) {
			if let Some(why) = not_on_windows(&case) {
				println!("SKIP {line}, {why}");
				continue;
			}
		}
		tally.ran += 1;
		let doc = fs::read_to_string(dir.join("doc.shcl")).unwrap();
		let ops = fs::read_to_string(dir.join("ops.txt")).unwrap();
		let got = run::<L>(&doc, &ops, &RunOptions { dir: Some(dir.clone()) });
		let want_path = case.join(format!("want.v{n}.txt"));
		match fs::read_to_string(&want_path) {
			Ok(want) if want == got => println!("PASS {line}"),
			Ok(_) => tally.fail(
				Some(line),
				&format!(
					"{name}/v{n}: output differs from {}:\n--- got\n{got}",
					want_path.display()
				),
			),
			Err(e) => tally.fail(
				Some(line),
				&format!("{name}/v{n}: no want file ({e}); the Go tests write them"),
			),
		}
	}
}

/// libtest's options that take a value, so the value isn't read as a filter.
const VALUE_OPTS: [&str; 7] = [
	"--test-threads",
	"--format",
	"--color",
	"--skip",
	"--logfile",
	"--shuffle-seed",
	"-Z",
];

fn main() -> ExitCode {
	let args: Vec<String> = std::env::args().skip(1).collect();
	let mut filter = None;
	let mut i = 0;
	while i < args.len() {
		if VALUE_OPTS.contains(&args[i].as_str()) {
			i += 1;
		} else if !args[i].starts_with('-') {
			filter = Some(args[i].as_str());
		}
		i += 1;
	}
	if args.iter().any(|a| a == "--list") {
		let mut names: Vec<_> = fs::read_dir(cases_dir())
			.unwrap()
			.map(|e| e.unwrap().file_name().to_string_lossy().into_owned())
			.filter(|n| filter.is_none_or(|f| n.contains(f)))
			.collect();
		names.sort();
		for name in names {
			println!("{name}: test");
		}
		return ExitCode::SUCCESS;
	}
	let root = Scratch::new("cases");
	let mut tally = Tally::default();
	run_all::<shclw::v3::Version>(root.path(), 3, filter, &mut tally);
	if tally.ran == 0 && filter.is_none() {
		tally.fail(None, "no cases ran");
	}
	if tally.failed > 0 {
		eprintln!("{} of the Rust cases failed", tally.failed);
		return ExitCode::FAILURE;
	}
	ExitCode::SUCCESS
}
