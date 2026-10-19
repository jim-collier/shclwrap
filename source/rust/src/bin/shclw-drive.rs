// Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
// SPDX-License-Identifier: MIT

//! Runs an ops script against a document with one shcl version, for the
//! tests that compare wrapper languages.
//!
//! ```text
//! shclw-drive VERSION DOCFILE [DIR] < SCRIPT
//! ```
//!
//! VERSION is 3. DIR is where the script's check and write ops find
//! and put files.

use std::io::Read as _;
use std::process::ExitCode;

use shclw::{run, RunOptions};

fn main() -> ExitCode {
	let args: Vec<String> = std::env::args().collect();
	if !(3..=4).contains(&args.len()) {
		eprintln!("usage: shclw-drive VERSION DOCFILE [DIR] < SCRIPT");
		return ExitCode::from(2);
	}
	let doc = match std::fs::read_to_string(&args[2]) {
		Ok(t) => t,
		Err(e) => {
			eprintln!("shclw-drive: {e}");
			return ExitCode::from(2);
		}
	};
	let mut script = String::new();
	if let Err(e) = std::io::stdin().read_to_string(&mut script) {
		eprintln!("shclw-drive: {e}");
		return ExitCode::from(2);
	}
	let opt = RunOptions {
		dir: args.get(3).map(Into::into),
	};
	let out = match args[1].as_str() {
		"3" => run::<shclw::v3::Version>(&doc, &script, &opt),
		v => {
			eprintln!("shclw-drive: no such version: {v}");
			return ExitCode::from(2);
		}
	};
	print!("{out}");
	ExitCode::SUCCESS
}
