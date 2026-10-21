// Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
// SPDX-License-Identifier: MIT

//! A small wrapper over several versions of shcl.
//!
//! Turn on the cargo feature for the shcl the program builds against,
//! `shcl3` for shcl 3.0, and use that module. A later module will have the
//! same calls, through the [`ShclDoc`] trait, so moving to another shcl
//! version is a change to the feature and the module name.
//!
//! ```ignore
//! use shclw::ShclDoc;
//! let doc = shclw::v3::open("app.shcl")?;
//! let port = doc.int("server.port", 8080);
//! ```

// Every call returns the one Error type, whose kinds are documented there,
// so a per-function errors section would say the same thing 30 times.
#![allow(clippy::missing_errors_doc)]
// Most calls are reads, and nothing is gained by marking each one.
#![allow(clippy::must_use_candidate)]

use std::fmt::{self, Write as _};
use std::fs;
use std::path::{Path, PathBuf};

mod info;
pub use info::{finish_info, Info, InfoEdit};

/// How a read went.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Status {
	Good,
	Empty,
	NotFound,
	BadType,
	Multiple,
	BadPath,
}

impl fmt::Display for Status {
	fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
		f.write_str(match self {
			Status::Good => "Good",
			Status::Empty => "Empty",
			Status::NotFound => "NotFound",
			Status::BadType => "BadType",
			Status::Multiple => "Multiple",
			Status::BadPath => "BadPath",
		})
	}
}

/// How forgiving a load is. Only `Strict` can fail one.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Strictness {
	Loose,
	#[default]
	Standard,
	Strict,
}

/// Severity of a diagnostic.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Severity {
	Error,
	Hint,
}

impl fmt::Display for Severity {
	fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
		f.write_str(match self {
			Severity::Error => "error",
			Severity::Hint => "hint",
		})
	}
}

/// One diagnostic from a load or a schema check. `line` is 1-based.
/// `layer` is the 1-based place in `open_layers`' list of the layer the line
/// is in, or 0 for a document that isn't layered.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Diag {
	pub line: usize,
	pub severity: Severity,
	pub code: &'static str,
	pub message: String,
	pub layer: usize,
}

impl fmt::Display for Diag {
	fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
		if self.layer > 0 {
			write!(f, "layer {} ", self.layer)?;
		}
		write!(f, "line {}: {} {}", self.line, self.code, self.message)
	}
}

/// A line in a loaded document. `layer` is as in `Diag`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub struct Spot {
	pub layer: usize,
	pub line: usize,
}

/// A name shcl kept one copy of, since a later copy had the same name and
/// value and was folded into it. `lines` is the kept copy's line, then each
/// later copy's. `path` reads the kept copy, as a `Node::path` does, and
/// `layer` is as in `Diag`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Merge {
	pub path: String,
	pub lines: Vec<usize>,
	pub layer: usize,
}

/// One layer for `open_layers`: a file, or text the program has, such as
/// its defaults or this run's command line.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Layer<'a> {
	File(&'a str),
	Text(&'a str),
}

/// A read with its status. Check `status` before trusting `value`. `text`
/// is the value as typed in the file, when there was one.
#[derive(Debug, Clone, PartialEq)]
pub struct Got<T> {
	pub value: T,
	pub status: Status,
	pub line: usize,
	pub text: String,
}

impl<T> Got<T> {
	/// True when the read found a usable value.
	pub fn ok(&self) -> bool {
		self.status == Status::Good
	}
}

/// One instance found by `each`. `path` reads this instance and no other;
/// add child names to it with `child`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Node {
	pub path: String,
	pub value: String,
	pub line: usize,
}

/// What went wrong, sorted into what a caller can act on.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ErrKind {
	NotFound,
	Io,
	Strict,
	Refused,
	BadPath,
	BadValue,
	TooNew,
	Unsupported,
	Ambiguous,
	BackupTaken,
	NoReadBack,
}

impl fmt::Display for ErrKind {
	fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
		f.write_str(match self {
			ErrKind::NotFound => "notfound",
			ErrKind::Io => "io",
			ErrKind::Strict => "strict",
			ErrKind::Refused => "refused",
			ErrKind::BadPath => "badpath",
			ErrKind::BadValue => "badvalue",
			ErrKind::TooNew => "toonew",
			ErrKind::Unsupported => "unsupported",
			ErrKind::Ambiguous => "ambiguous",
			ErrKind::BackupTaken => "backuptaken",
			ErrKind::NoReadBack => "noreadback",
		})
	}
}

/// The one error type every module returns. `diags` is set for a failed
/// strict load.
#[derive(Debug, Clone)]
pub struct Error {
	pub kind: ErrKind,
	pub path: String,
	pub msg: String,
	pub diags: Vec<Diag>,
}

impl Error {
	pub fn new(kind: ErrKind) -> Error {
		Error {
			kind,
			path: String::new(),
			msg: String::new(),
			diags: Vec::new(),
		}
	}

	#[must_use]
	pub fn path(mut self, path: &str) -> Error {
		path.clone_into(&mut self.path);
		self
	}

	#[must_use]
	pub fn msg(mut self, msg: impl Into<String>) -> Error {
		self.msg = msg.into();
		self
	}
}

impl fmt::Display for Error {
	fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
		write!(f, "shclw: {}", self.kind)?;
		if !self.path.is_empty() {
			write!(f, " {:?}", self.path)?;
		}
		if !self.msg.is_empty() {
			write!(f, ": {}", self.msg)?;
		}
		Ok(())
	}
}

impl std::error::Error for Error {}

/// What an upgrade made of a file. `from` is the format it was written for,
/// and `unsure` counts lines it couldn't convert for sure.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Upgraded {
	pub text: String,
	pub from: u32,
	pub changed: bool,
	pub unsure: usize,
	pub backup: String,
}

/// A value for `put`, with the Rust type folded down to the few shcl takes.
#[derive(Debug, Clone, PartialEq)]
pub enum Value {
	Int(i64),
	Float(f64),
	Bool(bool),
	Str(String),
	Strs(Vec<String>),
	Ints(Vec<i64>),
	Floats(Vec<f64>),
}

impl From<i64> for Value {
	fn from(v: i64) -> Value {
		Value::Int(v)
	}
}
impl From<i32> for Value {
	fn from(v: i32) -> Value {
		Value::Int(v.into())
	}
}
impl From<u32> for Value {
	fn from(v: u32) -> Value {
		Value::Int(v.into())
	}
}
impl From<f64> for Value {
	fn from(v: f64) -> Value {
		Value::Float(v)
	}
}
impl From<f32> for Value {
	fn from(v: f32) -> Value {
		// v.into() would write the f32's binary noise: 0.9 as 0.8999999761581421.
		Value::Float(shortest(v.into(), true).parse().unwrap_or(v.into()))
	}
}
impl From<bool> for Value {
	fn from(v: bool) -> Value {
		Value::Bool(v)
	}
}
impl From<&str> for Value {
	fn from(v: &str) -> Value {
		Value::Str(v.to_owned())
	}
}
impl From<String> for Value {
	fn from(v: String) -> Value {
		Value::Str(v)
	}
}
impl From<Vec<String>> for Value {
	fn from(v: Vec<String>) -> Value {
		Value::Strs(v)
	}
}
impl From<&[&str]> for Value {
	fn from(v: &[&str]) -> Value {
		Value::Strs(v.iter().map(|s| (*s).to_owned()).collect())
	}
}
impl From<Vec<i64>> for Value {
	fn from(v: Vec<i64>) -> Value {
		Value::Ints(v)
	}
}
impl From<&[i64]> for Value {
	fn from(v: &[i64]) -> Value {
		Value::Ints(v.to_vec())
	}
}
impl From<Vec<f64>> for Value {
	fn from(v: Vec<f64>) -> Value {
		Value::Floats(v)
	}
}
impl From<&[f64]> for Value {
	fn from(v: &[f64]) -> Value {
		Value::Floats(v.to_vec())
	}
}

/// What every module's document does. Bring it into scope to call the
/// methods: `use shclw::ShclDoc;`.
pub trait ShclDoc {
	/// The document as it would be saved.
	fn text(&self) -> String;
	/// The document in canonical form.
	fn canonical(&self) -> String;
	/// Saves `text()` atomically. The first write over a file `open`
	/// upgraded backs up the original first, as `upgrade_file` does.
	fn write(&mut self, path: &str) -> Result<(), Error>;
	/// Whether `write` puts the canonical form in place of the lines the
	/// document was loaded from, so lines no edit touched can change. A file
	/// `open` upgraded is rewritten until the first write over it.
	fn rewrites(&self) -> bool;
	/// The format `open` upgraded the file from, or 0 when it didn't
	/// upgrade it.
	fn upgraded_from(&self) -> u32;
	/// Where the first write over an upgraded file put the original, or ""
	/// before then.
	fn backup_name(&self) -> &str;
	/// What the load found.
	fn diags(&self) -> Vec<Diag>;
	/// Counts error diagnostics.
	fn errors(&self) -> usize;
	/// The lines the load couldn't use, which are the lines with an error,
	/// once each.
	fn unused(&self) -> Vec<Spot> {
		unused_of(&self.diags())
	}
	/// Each name the load kept one copy of, since a later copy had the same
	/// name and value, with every copy's line.
	fn merged(&self) -> Vec<Merge>;
	/// Validates this document against a schema document.
	fn check(&self, schema: &Self) -> Vec<Diag>;
	/// Takes out the hints for keys this schema says repeat.
	fn drop_declared(&self, diags: Vec<Diag>) -> Vec<Diag>;

	fn int(&self, path: &str, fallback: i64) -> i64;
	fn float(&self, path: &str, fallback: f64) -> f64;
	fn bool(&self, path: &str, fallback: bool) -> bool;
	fn str(&self, path: &str, fallback: &str) -> String;
	fn strs(&self, path: &str, fallback: &[&str]) -> Vec<String>;
	fn ints(&self, path: &str, fallback: &[i64]) -> Vec<i64>;
	fn floats(&self, path: &str, fallback: &[f64]) -> Vec<f64>;
	fn raw(&self, path: &str, fallback: &str) -> String;

	fn get_int(&self, path: &str) -> Got<i64>;
	fn get_float(&self, path: &str) -> Got<f64>;
	fn get_bool(&self, path: &str) -> Got<bool>;
	fn get_str(&self, path: &str) -> Got<String>;
	fn get_strs(&self, path: &str) -> Got<Vec<String>>;
	fn get_ints(&self, path: &str) -> Got<Vec<i64>>;
	fn get_floats(&self, path: &str) -> Got<Vec<f64>>;
	fn get_raw(&self, path: &str) -> Got<String>;

	fn has(&self, path: &str) -> bool;
	fn count(&self, path: &str) -> usize;
	/// Child names under `path` once each, in file order. With several
	/// instances it's every instance's children.
	fn names(&self, path: &str) -> Vec<String>;
	/// Every instance `path` names, each with a path that reads and edits it
	/// alone.
	fn each(&self, path: &str) -> Vec<Node>;
	fn line(&self, path: &str) -> usize;
	/// The line of each instance of `path`, in file order. An instance a
	/// put made has none, and is a 0 at the end.
	fn lines(&self, path: &str) -> Vec<usize> {
		in_file_order(self.each(path)).iter().map(|n| n.line).collect()
	}
	/// `path` narrowed to its first instance in the file, so a read or an
	/// edit takes that one where it would say `Multiple`. A path with no
	/// instances comes back as it is. One instance still gets its `each`
	/// path, which an edit takes where an inner step of `path` repeats.
	fn first(&self, path: &str) -> String {
		in_file_order(self.each(path))
			.first()
			.map_or_else(|| path.to_owned(), |n| n.path.clone())
	}
	/// `first` for the last instance in the file, the one that wins where a
	/// later line overrides an earlier one.
	fn last(&self, path: &str) -> String {
		in_file_order(self.each(path))
			.last()
			.map_or_else(|| path.to_owned(), |n| n.path.clone())
	}
	/// `base` with one more name on the end, quoted if it needs it.
	fn child(&self, base: &str, name: &str) -> String;

	fn put(&mut self, path: &str, v: impl Into<Value>) -> Result<(), Error>;
	/// Sets `path` to `text` read as shcl value syntax. Text that doesn't
	/// read back as typed, such as one with a trailing comment, is
	/// `NoReadBack`, with the edit left in place.
	fn put_syntax(&mut self, path: &str, text: &str) -> Result<(), Error>;
	/// Sets the comment above `path`.
	fn comment(&mut self, path: &str, text: &str) -> Result<(), Error>;
	/// Takes out `path` and everything under it, and says how many
	/// instances went.
	fn remove(&mut self, path: &str) -> usize;

	/// Puts the info block at the end of what the save writes, or with `on`
	/// false leaves it out. Either way an old block comes off first, in any
	/// spelling shcl has written, wherever it is. A file whose Format line
	/// names another format is refused, and so is one that ends in a raw
	/// block that never closes.
	fn info_block(&mut self, on: bool) -> Result<(), Error>;
	/// Adds the Format line at the end of what the save writes, unless a
	/// comment already names this format. It refuses what `info_block`
	/// refuses.
	fn stamp_format(&mut self) -> Result<(), Error>;
}

/// One module's package-level calls, for code that picks a shcl version at
/// run time or works with any of them.
pub trait Lib {
	type Doc: ShclDoc;
	const NAME: &'static str;
	const FORMAT_MAJOR: u32;
	fn parse(text: &str) -> Self::Doc;
	/// A load at `level`, with the document back even when it fails.
	fn parse_keep(text: &str, level: Strictness) -> (Self::Doc, Option<Error>);
	fn open_with(path: &str, level: Strictness) -> Result<Self::Doc, Error>;
	fn open_layers(layers: &[Layer]) -> Result<Self::Doc, Error>;
	fn format_of(text: &str) -> u32;
	fn upgrade(text: &str, from: u32) -> Result<Upgraded, Error>;
}

/// Reads back what a setter wrote, since shcl can take a value and read back
/// something else. See "What the wrapper evens out" in design.md.
// A float has to come back exactly, so no margin.
#[allow(clippy::float_cmp)]
pub fn verify_put<D: ShclDoc + ?Sized>(d: &D, path: &str, v: &Value) -> Result<(), Error> {
	let (status, same, empty) = match v {
		Value::Int(x) => {
			let g = d.get_int(path);
			(g.status, g.value == *x, false)
		}
		Value::Float(x) => {
			let g = d.get_float(path);
			(g.status, g.value == *x, false)
		}
		Value::Bool(x) => {
			let g = d.get_bool(path);
			(g.status, g.value == *x, false)
		}
		Value::Str(x) => {
			let g = d.get_str(path);
			(g.status, g.value == *x, false)
		}
		Value::Strs(x) => {
			let g = d.get_strs(path);
			(g.status, g.value == *x, x.is_empty())
		}
		Value::Ints(x) => {
			let g = d.get_ints(path);
			(g.status, g.value == *x, x.is_empty())
		}
		Value::Floats(x) => {
			let g = d.get_floats(path);
			(g.status, g.value == *x, x.is_empty())
		}
	};
	if status == Status::Good && same || status == Status::Empty && empty {
		return Ok(());
	}
	let msg = if status == Status::Good {
		"reads back as a different value".to_owned()
	} else {
		format!("reads back as {status}")
	};
	Err(Error::new(ErrKind::NoReadBack).path(path).msg(msg))
}

/// Refuses infinity and NaN, since no read takes them back. See "What the
/// wrapper evens out" in design.md.
pub fn check_value(path: &str, v: &Value) -> Result<(), Error> {
	let finite = match v {
		Value::Float(x) => x.is_finite(),
		Value::Floats(x) => x.iter().all(|f| f.is_finite()),
		_ => true,
	};
	if finite {
		return Ok(());
	}
	Err(Error::new(ErrKind::BadValue).path(path).msg("not a finite number"))
}

/// Makes an infinity or NaN read `BadType`, as 3.0 reads 1e400.
pub fn finite_float(mut g: Got<f64>) -> Got<f64> {
	if g.status == Status::Good && !g.value.is_finite() {
		g.status = Status::BadType;
		g.value = 0.0;
	}
	g
}

/// `finite_float` for an array. Like 3.0, the array keeps its length, with
/// 0 for each item that wasn't a number.
pub fn finite_floats(mut g: Got<Vec<f64>>) -> Got<Vec<f64>> {
	if g.status != Status::Good || g.value.iter().all(|f| f.is_finite()) {
		return g;
	}
	for f in &mut g.value {
		if !f.is_finite() {
			*f = 0.0;
		}
	}
	g.status = Status::BadType;
	g
}

/// Reads back what `put_syntax` set at `path`. `alone` is a new document
/// with the same text set at "k", and `line` is "k: text" parsed. shcl drops
/// a trailing comment from the text, so the two have to match. See "What the
/// wrapper evens out" in design.md.
pub fn verify_syntax<D: ShclDoc>(d: &D, path: &str, alone: &D, line: &D) -> Result<(), Error> {
	if alone.canonical() != line.canonical() {
		return Err(Error::new(ErrKind::NoReadBack)
			.path(path)
			.msg("the text doesn't read back as typed"));
	}
	let (got, want) = (d.get_str(path), line.get_str("k"));
	if got.status != want.status || got.value != want.value {
		return Err(Error::new(ErrKind::NoReadBack)
			.path(path)
			.msg("reads back as a different value"));
	}
	Ok(())
}

/// The lines with an error in `ds`, once each, by layer then line. An error
/// on no line, such as one about the whole file, has no line to give.
pub fn unused_of(ds: &[Diag]) -> Vec<Spot> {
	let mut out: Vec<Spot> = ds
		.iter()
		.filter(|g| g.severity == Severity::Error && g.line > 0)
		.map(|g| Spot {
			layer: g.layer,
			line: g.line,
		})
		.collect();
	out.sort_unstable();
	out.dedup();
	out
}

fn walk<D: ShclDoc>(x: &D, path: &str, see: &mut dyn FnMut(&str, &str, &Node) -> bool) {
	for name in x.names(path) {
		for n in x.each(&x.child(path, &name)) {
			if see(path, &name, &n) {
				walk(x, &n.path, see);
			}
		}
	}
}

fn probe_name(line: usize) -> String {
	format!("shclwprobe{line}x")
}

/// The copies shcl folded into an earlier one in `text`, apart or not
/// (shcl's H002 only says so when they're apart). A folded copy's line is
/// on no node, so each such line gets its name made unique and the text is
/// parsed again, where the copy then stands alone (design.md, Decisions).
pub fn merges_of<D: ShclDoc>(text: &str, parse: impl Fn(&str) -> D, layer: usize) -> Vec<Merge> {
	use std::collections::{HashMap, HashSet};
	let d = parse(text);
	let mut at: HashMap<usize, String> = HashMap::new();
	walk(&d, "", &mut |_, _, n| {
		if n.line > 0 {
			at.insert(n.line, n.path.clone());
		}
		true
	});
	let bad: HashSet<usize> = d
		.diags()
		.iter()
		.filter(|g| g.severity == Severity::Error)
		.map(|g| g.line)
		.collect();
	let mut probe: Vec<String> = Vec::new();
	let mut folded = HashSet::new();
	for (i, l) in text.split('\n').enumerate() {
		let n = i + 1;
		let body = l.trim_start_matches([' ', '\t']);
		let indent = &l[..l.len() - body.len()];
		let c = body.bytes().next().unwrap_or(b' ');
		if at.contains_key(&n) || bad.contains(&n) || body.is_empty() {
			probe.push(l.to_owned());
		} else if c == b'"' || c == b'\'' {
			probe.push(format!("{indent}{}{}{}", &body[..1], probe_name(n), &body[1..]));
			folded.insert(n);
		} else if c >= 0x80 || c.is_ascii_alphabetic() {
			probe.push(format!("{indent}{}{body}", probe_name(n)));
			folded.insert(n);
		} else {
			probe.push(l.to_owned());
		}
	}
	let mut out: Vec<Merge> = Vec::new();
	if folded.is_empty() {
		return out;
	}
	let p = parse(&probe.join("\n"));
	let mut slot: HashMap<String, usize> = HashMap::new();
	// The path in `d` of the node a probe node stands for.
	let mut orig: HashMap<String, String> = HashMap::from([(String::new(), String::new())]);
	walk(&p, "", &mut |parent, name, n| {
		if !folded.contains(&n.line) {
			let o = at.get(&n.line).cloned();
			let ok = o.is_some();
			orig.insert(n.path.clone(), o.unwrap_or_default());
			return ok;
		}
		let Some(up) = orig.get(parent).cloned() else {
			return false;
		};
		let pre = probe_name(n.line);
		let name = if name.to_ascii_lowercase().starts_with(&pre) {
			&name[pre.len()..]
		} else {
			name
		};
		let kept = d
			.each(&d.child(&up, name))
			.into_iter()
			.filter(|k| k.value == n.value && k.line > 0 && k.line < n.line)
			.max_by_key(|k| k.line);
		let Some(kept) = kept else { return false };
		orig.insert(n.path.clone(), kept.path.clone());
		// A dotted line folds each step; only its last one is the copy.
		for c in p.names(&n.path) {
			if p.each(&p.child(&n.path, &c)).iter().any(|k| k.line == n.line) {
				return true;
			}
		}
		let i = *slot.entry(kept.path.clone()).or_insert_with(|| {
			out.push(Merge {
				path: kept.path.clone(),
				lines: vec![kept.line],
				layer,
			});
			out.len() - 1
		});
		out[i].lines.push(n.line);
		true
	});
	for m in &mut out {
		m.lines.sort_unstable();
	}
	out.sort_by(|a, b| a.lines[0].cmp(&b.lines[0]).then_with(|| a.path.cmp(&b.path)));
	out
}

/// Sorts instances by line. `each` goes in tree order, which for a reopened
/// block isn't the order of the file. An instance a put made has no line,
/// and goes after the rest in `each` order.
fn in_file_order(mut nodes: Vec<Node>) -> Vec<Node> {
	nodes.sort_by_key(|n| if n.line == 0 { usize::MAX } else { n.line });
	nodes
}

/// Keeps the first of each name, compared without ASCII case, since shcl
/// matches names that way and lists one per line it was given on.
pub fn dedupe_names(names: Vec<String>) -> Vec<String> {
	let mut seen = std::collections::HashSet::new();
	names
		.into_iter()
		.filter(|n| seen.insert(n.to_ascii_lowercase()))
		.collect()
}

/// Reads a whole file, with a missing file as `NotFound`.
pub fn read_text(path: &str) -> Result<String, Error> {
	match fs::read(path) {
		Ok(b) => String::from_utf8(b).map_err(|_| Error::new(ErrKind::Io).path(path).msg("not valid UTF-8")),
		Err(e) if e.kind() == std::io::ErrorKind::NotFound => Err(Error::new(ErrKind::NotFound).path(path)),
		Err(e) => Err(Error::new(ErrKind::Io).path(path).msg(e.to_string())),
	}
}

/// Whether `path` still holds `was`, byte for byte.
pub fn unchanged(path: &str, was: &str) -> bool {
	fs::read(path).is_ok_and(|b| b == was.as_bytes())
}

/// Writes `data` over `path`, where `was` is the text last read from it. A
/// file that has changed since is left alone, checked before the backup and
/// again before the write. `backup` keeps `was` first and gives the copy's
/// name, or an empty name for none, which this returns. When the write
/// fails, the original stays or is put back from the backup, and then the
/// backup goes. Only when neither works is the backup kept, and the error
/// names it.
pub fn replace_read(
	path: &str,
	was: &str,
	data: &str,
	backup: impl FnOnce() -> Result<String, Error>,
	write: impl FnOnce(&str, &str) -> Result<(), Error>,
) -> Result<String, Error> {
	let changed = || {
		Error::new(ErrKind::Io)
			.path(path)
			.msg("changed since it was read; nothing written")
	};
	if !unchanged(path, was) {
		return Err(changed());
	}
	let name = backup()?;
	let drop_backup = || {
		if !name.is_empty() {
			let _ = fs::remove_file(&name); // a copy of what's still at path
		}
	};
	if !unchanged(path, was) {
		drop_backup();
		return Err(changed());
	}
	let Err(mut e) = write(path, data) else {
		return Ok(name);
	};
	if unchanged(path, was) {
		drop_backup();
		return Err(e);
	}
	if name.is_empty() {
		return Err(e);
	}
	// A replace that fails part way on Windows can leave nothing at path.
	let gone = matches!(fs::symlink_metadata(path), Err(m) if m.kind() == std::io::ErrorKind::NotFound);
	let note = if gone && fs::rename(&name, path).is_ok() {
		"the original was put back".to_owned()
	} else {
		format!("the original is {name}")
	};
	e.msg = if e.msg.is_empty() {
		note
	} else {
		format!("{}; {note}", e.msg)
	};
	Err(e)
}

fn strictness_of(level: &str) -> Strictness {
	match level {
		"loose" => Strictness::Loose,
		"strict" => Strictness::Strict,
		_ => Strictness::Standard,
	}
}

/// `name` with STAMP in place of the time in a backup's name.
fn mask_stamp(name: &str) -> String {
	let Some(at) = name.find("_backup_") else {
		return name.to_owned();
	};
	let start = at + "_backup_".len();
	match name[start..].find("_format-v") {
		Some(end) => format!("{}STAMP{}", &name[..start], &name[start + end..]),
		None => name.to_owned(),
	}
}

/// Writes a float the same way in every wrapper language: the shortest
/// digits that read back exactly, positional between 1e-5 and 1e17, else
/// with an exponent.
pub fn format_float(f: f64) -> String {
	if f.is_nan() {
		return "nan".to_owned();
	}
	if f.is_infinite() {
		return if f > 0.0 { "inf" } else { "-inf" }.to_owned();
	}
	let s = shortest(f, false);
	let (mant, exp) = s.split_once('e').unwrap_or((s.as_str(), "0"));
	let exp: i32 = exp.parse().unwrap_or(0);
	let neg = mant.starts_with('-');
	let digits: String = mant.trim_start_matches('-').chars().filter(|c| *c != '.').collect();
	assemble_float(neg, &digits, exp)
}

/// The shortest digits that read back as `f`, or as `f` taken as an f32
/// with `single`, in `{:e}` form, as Go's strconv gives them. Rust rounds an
/// exact tie at that length away from zero, and Go to even, except at a power
/// of 2 (`shclw_i_shortest` in the C wrapper has why).
fn shortest(f: f64, single: bool) -> String {
	#[allow(clippy::cast_possible_truncation)]
	let at = |prec: Option<usize>| match (single, prec) {
		(false, None) => format!("{f:e}"),
		(false, Some(p)) => format!("{f:.p$e}"),
		(true, None) => format!("{:e}", f as f32),
		(true, Some(p)) => format!("{:.p$e}", f as f32),
	};
	let s = at(None);
	#[allow(clippy::cast_possible_truncation)]
	let (pow2, field, odd_one) = if single {
		let b = (f as f32).to_bits();
		(b.trailing_zeros() >= 23, u64::from((b >> 23) & 0xff), 73)
	} else {
		let b = f.to_bits();
		(b.trailing_zeros() >= 52, (b >> 52) & 0x7ff, 998)
	};
	// Go leaves 2^-25, or as an f32 2^-54, at even.
	if pow2 && field != 0 && field != odd_one {
		return s;
	}
	let n = s.bytes().take_while(|&c| c != b'e').filter(u8::is_ascii_digit).count();
	let even = at(Some(n.saturating_sub(1)));
	let back = if single {
		even.parse::<f32>().map(f64::from)
	} else {
		even.parse::<f64>()
	};
	if back == Ok(f) {
		even
	} else {
		s
	}
}

fn assemble_float(neg: bool, digits: &str, exp: i32) -> String {
	let mut b = String::new();
	if neg {
		b.push('-');
	}
	let len = i32::try_from(digits.len()).unwrap_or(i32::MAX);
	if !(-5..17).contains(&exp) {
		b.push_str(&digits[..1]);
		if digits.len() > 1 {
			b.push('.');
			b.push_str(&digits[1..]);
		}
		b.push('e');
		b.push_str(&exp.to_string());
	} else if exp < 0 {
		b.push_str("0.");
		b.push_str(&"0".repeat(usize::try_from(-exp - 1).unwrap_or(0)));
		b.push_str(digits);
	} else if len <= exp + 1 {
		b.push_str(digits);
		b.push_str(&"0".repeat(usize::try_from(exp + 1 - len).unwrap_or(0)));
		b.push_str(".0");
	} else {
		let at = usize::try_from(exp + 1).unwrap_or(0);
		b.push_str(&digits[..at]);
		b.push('.');
		b.push_str(&digits[at..]);
	}
	b
}

/// What `run` may touch outside the document.
#[derive(Debug, Clone, Default)]
pub struct RunOptions {
	/// Where `check` reads schema files and `write` writes. `None` turns
	/// both ops off.
	pub dir: Option<PathBuf>,
}

/// Parses `input` with `L`, applies a script of ops to it, and returns what
/// the ops printed. The script and its output are the same in every
/// wrapper language, which is what the cross-language tests compare. The
/// ops are listed in project/design.md under "Ops scripts".
pub fn run<L: Lib>(input: &str, script: &str, opt: &RunOptions) -> String {
	let mut r = Runner::<L> {
		input,
		opt,
		doc: L::parse(input),
		out: String::new(),
	};
	for line in script.split('\n') {
		let line = line.trim_end_matches('\r');
		if line.trim().is_empty() || line.starts_with('#') {
			continue;
		}
		r.say(&format!("> {line}"));
		let f: Vec<String> = line.split('\t').map(unesc).collect();
		r.op(&f);
	}
	r.out
}

struct Runner<'a, L: Lib> {
	input: &'a str,
	opt: &'a RunOptions,
	doc: L::Doc,
	out: String,
}

impl<L: Lib> Runner<'_, L> {
	fn say(&mut self, s: &str) {
		self.out.push_str(s);
		self.out.push('\n');
	}

	fn op(&mut self, f: &[String]) {
		let arg = |i: usize| f.get(i).map_or("", String::as_str);
		let rest = |i: usize| f.get(i..).unwrap_or(&[]).to_vec();
		match arg(0) {
			"parse" => self.parse(arg(1)),
			"text" => self.block(&self.doc.text()),
			"canonical" => self.block(&self.doc.canonical()),
			"rewrites" => self.say(&self.doc.rewrites().to_string()),
			"diags" => self.diags(&self.doc.diags()),
			"errors" => self.say(&self.doc.errors().to_string()),
			"unused" => {
				let spots = self.doc.unused();
				let items = spots
					.iter()
					.map(|s| spot_text(s.line, s.layer))
					.collect::<Vec<_>>()
					.join("\t");
				self.say(&counted(spots.len(), &items));
			}
			"merged" => {
				for m in self.doc.merged() {
					let mut items = vec![esc(&m.path)];
					items.extend(m.lines.iter().map(|&n| spot_text(n, m.layer)));
					self.say(&items.join("\t"));
				}
			}
			"get" => {
				let s = self.get(arg(1), arg(2));
				self.say(&s);
			}
			"or" => self.or(arg(1), arg(2), &rest(3)),
			"has" => self.say(&self.doc.has(arg(1)).to_string()),
			"count" => self.say(&self.doc.count(arg(1)).to_string()),
			"line" => self.say(&self.doc.line(arg(1)).to_string()),
			"lines" => {
				let lines = self.doc.lines(arg(1));
				let items = lines.iter().map(ToString::to_string).collect::<Vec<_>>().join("\t");
				self.say(&counted(lines.len(), &items));
			}
			"first" => {
				let s = self.get(arg(1), &self.doc.first(arg(2)));
				self.say(&s);
			}
			"last" => {
				let s = self.get(arg(1), &self.doc.last(arg(2)));
				self.say(&s);
			}
			"names" => self.say(&join_esc(&self.doc.names(arg(1)))),
			"each" => {
				for (i, n) in self.doc.each(arg(1)).iter().enumerate() {
					self.say(&format!("{i}\t{}\t{}", n.line, esc(&n.value)));
				}
			}
			"eachget" => {
				for (i, n) in self.doc.each(arg(2)).iter().enumerate() {
					let p = self.doc.child(&n.path, arg(3));
					let s = self.get(arg(1), &p);
					self.say(&format!("{i}\t{s}"));
				}
			}
			"put" => match parse_value(arg(1), &rest(3)) {
				Some(v) => {
					let r = self.doc.put(arg(2), v);
					self.result(r.err());
				}
				None => self.say("error badvalue"),
			},
			"syntax" => {
				let r = self.doc.put_syntax(arg(1), arg(2));
				self.result(r.err());
			}
			"comment" => {
				let r = self.doc.comment(arg(1), arg(2));
				self.result(r.err());
			}
			"remove" => {
				let n = self.doc.remove(arg(1));
				self.say(&n.to_string());
			}
			"info" => match arg(1) {
				"on" | "off" => {
					let r = self.doc.info_block(arg(1) == "on");
					self.result(r.err());
				}
				_ => self.say("error badvalue"),
			},
			"stamp" => {
				let r = self.doc.stamp_format();
				self.result(r.err());
			}
			"check" => self.check(arg(1), arg(2)),
			"format" => self.say(&L::format_of(self.input).to_string()),
			"upgrade" => self.upgrade(arg(1)),
			"write" => self.write(arg(1)),
			"open" => self.open(arg(1), arg(2)),
			"layers" => self.layers(&rest(1)),
			"upgraded" => self.upgraded(),
			"files" => self.files(),
			_ => self.say("error unknownop"),
		}
	}

	fn parse(&mut self, level: &str) {
		let (doc, err) = L::parse_keep(self.input, strictness_of(level));
		self.doc = doc;
		self.result(err);
	}

	/// An error's diagnostics go after it, and the document is then empty,
	/// as in C, which has no list on its error.
	fn open(&mut self, name: &str, level: &str) {
		let Some(path) = self.file(name) else { return };
		match L::open_with(&path.to_string_lossy(), strictness_of(level)) {
			Ok(d) => {
				self.doc = d;
				self.say("ok");
			}
			Err(e) => {
				self.say(&format!("error {}", e.kind));
				self.diags(&e.diags);
				self.doc = L::parse("");
			}
		}
	}

	/// File names, and text for a text layer after a "=".
	fn layers(&mut self, specs: &[String]) {
		let mut paths = Vec::with_capacity(specs.len());
		for spec in specs {
			if let Some(text) = spec.strip_prefix('=') {
				paths.push((false, text.to_owned()));
				continue;
			}
			let Some(path) = self.file(spec) else { return };
			paths.push((true, path.to_string_lossy().into_owned()));
		}
		let layers: Vec<Layer> = paths
			.iter()
			.map(|(file, s)| if *file { Layer::File(s) } else { Layer::Text(s) })
			.collect();
		match L::open_layers(&layers) {
			Ok(d) => {
				self.doc = d;
				self.say("ok");
			}
			Err(e) => {
				self.say(&format!("error {}", e.kind));
				self.diags(&e.diags);
				self.doc = L::parse("");
			}
		}
	}

	fn upgraded(&mut self) {
		let from = self.doc.upgraded_from();
		let name = self.doc.backup_name().to_owned();
		if name.is_empty() {
			return self.say(&format!("from {from} backup none"));
		}
		let base = Path::new(&name)
			.file_name()
			.map(|b| b.to_string_lossy().into_owned())
			.unwrap_or_default();
		self.say(&format!("from {from} backup {}", esc(&mask_stamp(&base))));
		match read_text(&name) {
			Ok(t) => self.block(&t),
			Err(e) => self.result(Some(e)),
		}
	}

	fn files(&mut self) {
		let Some(dir) = &self.opt.dir else {
			return self.say("error unsupported");
		};
		let mut names: Vec<String> = match fs::read_dir(dir) {
			Ok(rd) => rd
				.filter_map(Result::ok)
				.map(|e| e.file_name().to_string_lossy().into_owned())
				.collect(),
			Err(e) => return self.result(Some(Error::new(ErrKind::Io).msg(e.to_string()))),
		};
		names.sort();
		for n in names {
			self.say(&esc(&mask_stamp(&n)));
		}
	}

	fn result(&mut self, err: Option<Error>) {
		match err {
			None => self.say("ok"),
			Some(e) => self.say(&format!("error {}", e.kind)),
		}
	}

	fn diags(&mut self, ds: &[Diag]) {
		for d in ds {
			let line = spot_text(d.line, d.layer);
			self.say(&format!("{line}\t{}\t{}\t{}", d.severity, d.code, esc(&d.message)));
		}
	}

	/// Prints multi-line text one line per output line, so a golden file
	/// diff shows which line changed.
	fn block(&mut self, text: &str) {
		let mut lines: Vec<&str> = text.split('\n').collect();
		if lines.last() == Some(&"") {
			lines.pop();
		}
		for l in lines {
			self.say(&format!("| {}", esc(l)));
		}
		if !text.is_empty() && !text.ends_with('\n') {
			self.say("(no newline at end)");
		}
	}

	fn get(&self, typ: &str, path: &str) -> String {
		let d = &self.doc;
		let head = |s: Status, line: usize| format!("{s}\t{line}");
		match typ {
			"int" => {
				let g = d.get_int(path);
				format!("{}\t{}", head(g.status, g.line), g.value)
			}
			"float" => {
				let g = d.get_float(path);
				format!("{}\t{}", head(g.status, g.line), format_float(g.value))
			}
			"bool" => {
				let g = d.get_bool(path);
				format!("{}\t{}", head(g.status, g.line), g.value)
			}
			"str" => {
				let g = d.get_str(path);
				format!("{}\t{}", head(g.status, g.line), esc(&g.value))
			}
			"raw" => {
				let g = d.get_raw(path);
				format!("{}\t{}", head(g.status, g.line), esc(&g.value))
			}
			"strs" => {
				let g = d.get_strs(path);
				format!(
					"{}\t{}",
					head(g.status, g.line),
					counted(g.value.len(), &join_esc(&g.value))
				)
			}
			"ints" => {
				let g = d.get_ints(path);
				format!(
					"{}\t{}",
					head(g.status, g.line),
					counted(g.value.len(), &join_ints(&g.value))
				)
			}
			"floats" => {
				let g = d.get_floats(path);
				format!(
					"{}\t{}",
					head(g.status, g.line),
					counted(g.value.len(), &join_floats(&g.value))
				)
			}
			_ => "error badtype".to_owned(),
		}
	}

	fn or(&mut self, typ: &str, path: &str, fallback: &[String]) {
		let first = fallback.first().map_or("", String::as_str);
		let d = &self.doc;
		let s = match typ {
			"int" => first.parse::<i64>().ok().map(|n| d.int(path, n).to_string()),
			"float" => parse_float(first).map(|x| format_float(d.float(path, x))),
			"bool" => Some(d.bool(path, first == "true").to_string()),
			"str" => Some(esc(&d.str(path, first))),
			"raw" => Some(esc(&d.raw(path, first))),
			"strs" => {
				let fb: Vec<&str> = fallback.iter().map(String::as_str).collect();
				let v = d.strs(path, &fb);
				Some(counted(v.len(), &join_esc(&v)))
			}
			"ints" => match parse_value("ints", fallback) {
				Some(Value::Ints(fb)) => {
					let v = d.ints(path, &fb);
					Some(counted(v.len(), &join_ints(&v)))
				}
				_ => None,
			},
			"floats" => match parse_value("floats", fallback) {
				Some(Value::Floats(fb)) => {
					let v = d.floats(path, &fb);
					Some(counted(v.len(), &join_floats(&v)))
				}
				_ => None,
			},
			_ => {
				self.say("error badtype");
				return;
			}
		};
		self.say(&s.unwrap_or_else(|| "error badvalue".to_owned()));
	}

	/// Prints a schema check. Mode "all" adds the load's own diagnostics
	/// first, and "drop" does that and then drops what the schema declares.
	fn check(&mut self, name: &str, mode: &str) {
		let Some(path) = self.file(name) else { return };
		let text = match read_text(&path.to_string_lossy()) {
			Ok(t) => t,
			Err(e) => return self.result(Some(e)),
		};
		let schema = L::parse(&text);
		let mut diags = self.doc.check(&schema);
		if mode == "all" || mode == "drop" {
			let mut all = self.doc.diags();
			all.append(&mut diags);
			diags = all;
		}
		if mode == "drop" {
			diags = schema.drop_declared(diags);
		}
		self.diags(&diags);
	}

	fn upgrade(&mut self, from: &str) {
		let n = if from.is_empty() {
			Some(0)
		} else {
			from.parse::<u32>().ok()
		};
		let Some(n) = n else { return self.say("error badvalue") };
		match L::upgrade(self.input, n) {
			Ok(u) => {
				self.say(&format!("from {} changed {} unsure {}", u.from, u.changed, u.unsure));
				self.block(&u.text);
			}
			Err(e) => self.result(Some(e)),
		}
	}

	fn write(&mut self, name: &str) {
		let Some(path) = self.file(name) else { return };
		let path = path.to_string_lossy().into_owned();
		if let Err(e) = self.doc.write(&path) {
			return self.result(Some(e));
		}
		match read_text(&path) {
			Ok(t) => {
				self.say("ok");
				self.block(&t);
			}
			Err(e) => self.result(Some(e)),
		}
	}

	/// Keeps a script's file names inside `RunOptions::dir`.
	fn file(&mut self, name: &str) -> Option<PathBuf> {
		let plain = !name.is_empty() && name != ".." && Path::new(name).file_name().is_some_and(|b| b == name);
		match &self.opt.dir {
			Some(dir) if plain => Some(dir.join(name)),
			_ => {
				self.say("error unsupported");
				None
			}
		}
	}
}

fn parse_value(typ: &str, f: &[String]) -> Option<Value> {
	let first = f.first().map_or("", String::as_str);
	match typ {
		"int" => first.parse::<i64>().ok().map(Value::Int),
		"float" => parse_float(first).map(Value::Float),
		"bool" => match first {
			"true" => Some(Value::Bool(true)),
			"false" => Some(Value::Bool(false)),
			_ => None,
		},
		"str" => Some(Value::Str(first.to_owned())),
		"strs" => Some(Value::Strs(f.to_vec())),
		"ints" => f
			.iter()
			.map(|s| s.parse::<i64>().ok())
			.collect::<Option<Vec<_>>>()
			.map(Value::Ints),
		"floats" => f
			.iter()
			.map(|s| parse_float(s))
			.collect::<Option<Vec<_>>>()
			.map(Value::Floats),
		_ => None,
	}
}

/// Takes plain decimal and exponent forms, plus nan, inf, and -inf as
/// `format_float` writes them. Each language's own parser takes a different
/// set of spellings for those. A number too big is infinity.
fn parse_float(s: &str) -> Option<f64> {
	match s {
		"nan" => return Some(f64::NAN),
		"inf" => return Some(f64::INFINITY),
		"-inf" => return Some(f64::NEG_INFINITY),
		_ => {}
	}
	let odd = |c: char| c.is_ascii_alphabetic() && c != 'e' && c != 'E' || c == '_';
	if s.is_empty() || s.contains(odd) {
		return None;
	}
	s.parse::<f64>().ok()
}

/// Undoes \t, \n, \r, and \\ in a script field.
fn unesc(s: &str) -> String {
	if !s.contains('\\') {
		return s.to_owned();
	}
	let mut b = String::with_capacity(s.len());
	let mut it = s.chars();
	while let Some(c) = it.next() {
		if c != '\\' {
			b.push(c);
			continue;
		}
		match it.next() {
			Some('t') => b.push('\t'),
			Some('n') => b.push('\n'),
			Some('r') => b.push('\r'),
			Some('\\') | None => b.push('\\'),
			Some(o) => {
				b.push('\\');
				b.push(o);
			}
		}
	}
	b
}

/// Makes a value safe for one tab-separated output field.
fn esc(s: &str) -> String {
	let mut b = String::with_capacity(s.len());
	for c in s.chars() {
		match c {
			'\\' => b.push_str("\\\\"),
			'\t' => b.push_str("\\t"),
			'\n' => b.push_str("\\n"),
			'\r' => b.push_str("\\r"),
			c if (c as u32) < 0x20 || c as u32 == 0x7f => {
				let _ = write!(b, "\\x{:02x}", c as u32); // writing to a String can't fail
			}
			c => b.push(c),
		}
	}
	b
}

/// Puts an array's length before its items, so `[]` and `[""]` print
/// differently.
/// A line, with its layer in front on a layered document.
fn spot_text(line: usize, layer: usize) -> String {
	if layer > 0 {
		format!("{layer}:{line}")
	} else {
		line.to_string()
	}
}

fn counted(n: usize, items: &str) -> String {
	if n == 0 {
		"0".to_owned()
	} else {
		format!("{n}\t{items}")
	}
}

fn join_esc(v: &[String]) -> String {
	v.iter().map(|s| esc(s)).collect::<Vec<_>>().join("\t")
}

fn join_ints(v: &[i64]) -> String {
	v.iter().map(ToString::to_string).collect::<Vec<_>>().join("\t")
}

fn join_floats(v: &[f64]) -> String {
	v.iter().map(|x| format_float(*x)).collect::<Vec<_>>().join("\t")
}

#[cfg(feature = "shcl3")]
pub mod v3;
