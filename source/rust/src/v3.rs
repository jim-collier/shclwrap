// Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
// SPDX-License-Identifier: MIT

//! The wrapper over shcl 3.0.
//!
//! Documents keep their lines as typed, so `text` and `write` give back what
//! was read plus the edits, where 3.0 can manage that, and the canonical
//! form where it can't. `rewrites` says which. `write` refuses with
//! `Refused` rather than drop a line.

/// The shcl crate underneath, so code using `inner()` can name its types.
pub use shcl3 as shcl;

use crate::{
	Diag, ErrKind, Error, Got, Layer, Merge, Node, Severity, ShclDoc, Status, Strictness, Upgraded, Value, WriteOpts,
};

/// The shcl release this module wraps.
pub const SHCL_VERSION: &str = "3.0.0-dev+ea0998fa";

/// The newest file format this shcl reads.
pub const FORMAT_MAJOR: u32 = shcl::FORMAT_MAJOR;

/// A parsed document.
#[derive(Debug)]
pub struct Doc {
	d: shcl::Document,
	info: crate::Info,
	stamp: bool,
	up: Option<UpgradedFile>,
	file: Option<ReadFile>,
	lay: Option<Layers>,
	// The text as loaded, for `merged`.
	src: String,
	// The newer format the text names, which makes it read-only.
	newer: u32,
}

/// What `open_layers` read. `d` is then their merge, built again after each
/// edit, and edits and saves go to `docs[edit]`.
#[derive(Debug)]
struct Layers {
	// None for a file that wasn't there.
	docs: Vec<Option<Doc>>,
	edit: usize,
}

/// The format `open` upgraded the file from, and where the first write over
/// it put the original.
#[derive(Debug)]
struct UpgradedFile {
	from: u32,
	backup: String,
}

/// The file `open` read, as it was then, so `write` can tell it changed
/// since. After a write over it, it's what that write left.
#[derive(Debug)]
struct ReadFile {
	path: String,
	id: Option<FileId>,
	text: String,
	// An `open_layers` user file that wasn't there.
	gone: bool,
}

#[cfg(unix)]
type FileId = (u64, u64);
#[cfg(not(unix))]
type FileId = std::path::PathBuf;

#[cfg(unix)]
fn file_id(path: &str) -> Option<FileId> {
	use std::os::unix::fs::MetadataExt;
	std::fs::metadata(path).ok().map(|m| (m.dev(), m.ino()))
}

#[cfg(not(unix))]
fn file_id(path: &str) -> Option<FileId> {
	std::fs::canonicalize(path).ok()
}

impl ReadFile {
	/// Whether `path` names the file `open` read: the same path, or the same
	/// file by another name.
	fn over(&self, path: &str) -> bool {
		let same = |a: &str, b: &str| match (std::path::absolute(a), std::path::absolute(b)) {
			(Ok(a), Ok(b)) => a == b,
			_ => false,
		};
		same(path, &self.path) || (self.id.is_some() && file_id(path) == self.id)
	}

	/// Writes `text` over the file `open` read, unless it changed since.
	/// Only the moment between the last check and the rename is left open
	/// (design.md, Loading and saving).
	fn replace(
		&self,
		path: &str,
		text: &str,
		backup: impl FnOnce() -> Result<String, Error>,
		force: bool,
	) -> Result<String, Error> {
		let now;
		let mut was = self.text.as_str();
		if force {
			let Ok(b) = std::fs::read_to_string(path) else {
				return write_atomic(path, text).map(|()| String::new());
			};
			now = b;
			was = &now;
		} else if self.gone {
			if std::fs::symlink_metadata(path).is_ok() {
				return Err(crate::changed(path));
			}
			return write_atomic(path, text).map(|()| String::new());
		}
		crate::replace_read(path, was, text, backup, write_atomic)
	}
}

/// This module's calls as a [`crate::Lib`].
#[derive(Debug)]
pub struct Version;

impl crate::Lib for Version {
	type Doc = Doc;
	const NAME: &'static str = "shcl 3.0.0-dev+ea0998fa";
	const FORMAT_MAJOR: u32 = FORMAT_MAJOR;
	fn parse(text: &str) -> Doc {
		parse(text)
	}
	fn parse_keep(text: &str, level: Strictness) -> (Doc, Option<Error>) {
		parse_keep(text, level)
	}
	fn open_with(path: &str, level: Strictness) -> Result<Doc, Error> {
		open_with(path, level)
	}
	fn open_layers(layers: &[Layer]) -> Result<Doc, Error> {
		open_layers(layers)
	}
	fn format_of(text: &str) -> u32 {
		format_of(text)
	}
	fn upgrade(text: &str, from: u32) -> Result<Upgraded, Error> {
		upgrade(text, from)
	}
}

/// An empty document. It keeps lines like a loaded one, so it has none to
/// lose and `rewrites` says false.
pub fn new() -> Doc {
	parse("")
}

/// Never fails. Problems are in `diags`.
pub fn parse(text: &str) -> Doc {
	parse_keep(text, Strictness::Standard).0
}

/// Fails only at `Strict`, with `ErrKind::Strict`.
pub fn parse_with(text: &str, level: Strictness) -> Result<Doc, Error> {
	match parse_keep(text, level) {
		(d, None) => Ok(d),
		(_, Some(e)) => Err(e),
	}
}

fn parse_keep(text: &str, level: Strictness) -> (Doc, Option<Error>) {
	match shcl::Document::parse_keep_lines(text, strictness(level)) {
		Ok(d) => (doc(d, text), None),
		Err(le) => {
			let mut e = Error::new(ErrKind::Strict).msg(le.to_string());
			e.diags = diags(&le.diagnostics);
			(doc(le.document, text), Some(e))
		}
	}
}

fn doc(d: shcl::Document, text: &str) -> Doc {
	Doc {
		d,
		info: crate::Info::AsIs,
		stamp: false,
		up: None,
		file: None,
		lay: None,
		src: text.to_owned(),
		newer: Some(format_of(text)).filter(|&f| f > FORMAT_MAJOR).unwrap_or(0),
	}
}

/// Reads `layers` in order, lowest first, such as defaults, then a site
/// file, then the user's, and reads as their merge: a later layer's value,
/// array, or raw block takes the place of a lower one's, and blocks merge
/// name by name, as shcl's `merge` does. A missing file is skipped.
///
/// Edits and saves go to the last file layer, so `text`, `write`, and the
/// rest are that layer's, and `write` writes only it. Text layers after it,
/// such as this run's command line, still win reads. Each file is opened as
/// `open` does, and the first error is returned. `diags` are every layer's,
/// with `layer` set (design.md, Decisions).
pub fn open_layers(layers: &[Layer]) -> Result<Doc, Error> {
	if layers.is_empty() {
		return Ok(new());
	}
	let edit = layers
		.iter()
		.rposition(|l| matches!(l, Layer::File(_)))
		.unwrap_or(layers.len() - 1);
	let mut docs = Vec::with_capacity(layers.len());
	for (i, l) in layers.iter().enumerate() {
		match *l {
			Layer::Text(text) => docs.push(Some(parse(text))),
			Layer::File(path) => match open(path) {
				Ok(d) => docs.push(Some(d)),
				Err(e) if e.kind == ErrKind::NotFound => docs.push((i == edit).then(|| {
					let mut d = new();
					d.file = Some(ReadFile {
						path: path.to_owned(),
						id: None,
						text: String::new(),
						gone: true,
					});
					d
				})),
				Err(mut e) => {
					for g in &mut e.diags {
						g.layer = i + 1;
					}
					return Err(e);
				}
			},
		}
	}
	let mut d = new();
	d.lay = Some(Layers { docs, edit });
	d.remerge();
	Ok(d)
}

/// Reads and parses a file. A missing file is `NotFound`. A file written for
/// a newer format is read as this shcl reads it, and is read-only: `write`
/// on it is `TooNew`. An older file is upgraded in
/// memory, as `upgrade` with `from` 0 does, and `upgraded_from` says so.
/// Nothing is written until `write`, and the first write over the file backs
/// it up. A 2.x file that reads two ways is `Ambiguous`, and one shcl 2.0
/// reports errors on, or that can't be made to read as 2.0 read it, is
/// `Refused`, with the lines in `diags`.
pub fn open(path: &str) -> Result<Doc, Error> {
	open_with(path, Strictness::Standard)
}

/// `open` at a given strictness, which applies to the upgraded text.
pub fn open_with(path: &str, level: Strictness) -> Result<Doc, Error> {
	let text = crate::read_text(path)?;
	// A file naming this format or a newer one skips the upgrade's parse.
	let u = if format_of(&text) < FORMAT_MAJOR {
		Some(upgrade(&text, 0).map_err(|e| e.path(path))?).filter(|u| u.changed)
	} else {
		None
	};
	let mut d = parse_with(u.as_ref().map_or(&text, |u| &u.text), level).map_err(|e| e.path(path))?;
	d.up = u.map(|u| UpgradedFile {
		from: u.from,
		backup: String::new(),
	});
	d.file = Some(ReadFile {
		path: path.to_owned(),
		id: file_id(path),
		text,
		gone: false,
	});
	Ok(d)
}

/// The format a file's Format line names, or 0 with none.
pub fn format_of(text: &str) -> u32 {
	shcl::format_version(text).unwrap_or(0)
}

/// Converts a file written for an older format. With `from` 0 the file's
/// Format line decides. A file with none is taken as 2.x when it has 2.x's
/// info block, or errors on 3.0 that shcl 2.0 doesn't have, and otherwise as
/// current. Pass 2 for a file known to be from shcl 2.x or older. A 2.x file
/// that reads two ways is `Ambiguous`, and one shcl 2.0 reports errors on is
/// `Refused`, with those errors in `diags`. A
/// 2.x file comes out reading what 2.0 read, backslash escapes included, and
/// one that can't is `Refused` too. 2.x's block is swapped for the current
/// one where it sits, and the lines end as the file's did.
pub fn upgrade(text: &str, from: u32) -> Result<Upgraded, Error> {
	let f = format_of(text).max(from);
	if f > FORMAT_MAJOR {
		return Err(too_new(f));
	}
	let e = info_edit();
	// The new block goes where 2.x's sat, so a mark shcl leaves be stands in.
	let (input, banner) = if format_of(text) == 0 {
		crate::info::old_block(text, &e)
	} else {
		(text.to_owned(), false)
	};
	// A file with no Format line that has 2.x's block is 2.x (design.md,
	// Decisions).
	let from = if banner && from == 0 { 2 } else { from };
	let from_v2 = from > 0 && from < FORMAT_MAJOR;
	let u = shcl::upgrade(&input, from_v2);
	// shcl says 2 for a file with no Format line, even one it leaves alone.
	let taken_as_current = u.current && !from_v2 && format_of(text) == 0;
	// Only a file taken as 2.x is read the 2.x way.
	let old = !u.current || (from_v2 && u.format < FORMAT_MAJOR);
	let ambiguous = u.ambiguous;
	let mut out = upgraded(u);
	if taken_as_current {
		out.from = FORMAT_MAJOR;
	}
	if !old {
		text.clone_into(&mut out.text);
		return Ok(out);
	}
	// Nothing in this file says it's 2.x, only its errors on 3.0. With errors
	// on 2.0 too, it's a current file with a typo (design.md, Decisions).
	let old = shcl2::Document::parse(text);
	let errors = old_errors(&old);
	if errors.is_some() && !from_v2 && format_of(text) == 0 {
		return Ok(Upgraded {
			text: text.to_owned(),
			from: FORMAT_MAJOR,
			..Upgraded::default()
		});
	}
	if ambiguous > 0 {
		return Err(Error::new(ErrKind::Ambiguous).msg(format!("{ambiguous} lines read two ways")));
	}
	if let Some(e) = errors {
		return Err(e);
	}
	let fixed = carry_reads(&old, &out.text, format_of(text) == 1)?;
	out.text = if banner {
		crate::info::place_block(&crate::info::same_ends(text, &fixed, &e), input != text, &e)?
	} else if !out.changed && fixed == out.text {
		return Ok(out);
	} else if !out.changed {
		// shcl left this one as it was, so it has no Format line yet, and the
		// next upgrade would read the escapes a second time.
		crate::finish_info(&fixed, crate::Info::On, false, &e)?
	} else {
		crate::info::same_ends(text, &fixed, &e)
	};
	out.changed = out.text != text;
	Ok(out)
}

/// `upgrade` on a file. When it changes the file it first copies the
/// original beside it, and `backup` names the copy. A file that changed
/// after it was read is left alone, and a failed write leaves the original
/// in place (`crate::replace_read`).
pub fn upgrade_file(path: &str, from: u32) -> Result<Upgraded, Error> {
	if from > FORMAT_MAJOR {
		return Err(too_new(from).path(path));
	}
	// A FIFO would block the read, and the write would put a file in place
	// of a device.
	if std::fs::metadata(path).is_ok_and(|m| !m.is_file()) {
		return Err(Error::new(ErrKind::Io).path(path).msg("not a regular file"));
	}
	// One read for the 2.0 check, the conversion, and the backup. shcl's own
	// upgrade_file would read the file a second time, after the check.
	let text = crate::read_text(path)?;
	let mut u = upgrade(&text, from).map_err(|e| e.path(path))?;
	if !u.changed {
		return Ok(u);
	}
	let write = |path: &str, data: &str| {
		shcl::write_file_atomic(path, data).map_err(|e| Error::new(ErrKind::Io).path(path).msg(e.to_string()))
	};
	u.backup = crate::replace_read(path, &text, &u.text, || keep_original(path, &text, u.from), write)?;
	Ok(u)
}

/// Copies `text`, what `path` held when it was read, beside it under shcl's
/// backup name for format `from`.
fn keep_original(path: &str, text: &str, from: u32) -> Result<String, Error> {
	shcl::write_backup(path, text, from).map_err(|e| {
		let kind = match &e {
			shcl::UpgradeError::BackupTaken(_) => ErrKind::BackupTaken,
			shcl::UpgradeError::NotFound(_) | shcl::UpgradeError::Ambiguous { .. } | shcl::UpgradeError::Io(_) => {
				ErrKind::Io
			}
		};
		Error::new(kind).path(path).msg(e.to_string())
	})
}

fn write_atomic(path: &str, text: &str) -> Result<(), Error> {
	shcl::write_file_atomic(path, text).map_err(|w| {
		let kind = if w.status == shcl::WriteStatus::NotFound {
			ErrKind::NotFound
		} else {
			ErrKind::Io
		};
		Error::new(kind).path(path).msg(w.message)
	})
}

/// Refuses a file taken as 2.x that shcl 2.0 reports errors on. The old
/// program read those lines as it repaired them, or not at all, and an open
/// raw block took the rest of the file. The upgrade would turn that into a
/// clean file that means something else (design.md, Format and upgrade).
fn old_errors(old: &shcl2::Document) -> Option<Error> {
	let errs: Vec<Diag> = old
		.diagnostics()
		.iter()
		.filter(|d| matches!(d.severity, shcl2::Severity::Error))
		.map(|d| Diag {
			line: d.line,
			severity: Severity::Error,
			code: d.code,
			message: d.message.clone(),
			layer: 0,
		})
		.collect();
	if errs.is_empty() {
		return None;
	}
	let mut lines: Vec<usize> = errs.iter().map(|d| d.line).collect();
	lines.sort_unstable();
	lines.dedup();
	let mut e = Error::new(ErrKind::Refused).msg(format!(
		"shcl 2.0 reports errors on {} lines; fix them and upgrade again",
		lines.len()
	));
	e.diags = errs;
	Some(e)
}

/// Makes `up`, the upgraded text, read each value the way shcl 2.0 read it
/// in `old`. shcl's migrate leaves a 2.x backslash escape as typed, and 3.0
/// reads it as text, so `C:\\new` was `C:\new` and would now be `C:\\new`
/// (2026101910455900). Each value with an escape in it is set again, then
/// every path 2.0 reads one value at is read back against 2.0. A name 2.0 read
/// with an escape resolved can't be renamed here, so it's refused, unless the
/// file names format 1, since 1.2 read names as typed. So is a value that
/// still reads differently.
fn carry_reads(old: &shcl2::Document, up: &str, names_as_typed: bool) -> Result<String, Error> {
	let mut d = shcl::Document::parse_keep_lines(up, shcl::Strictness::Standard).unwrap_or_else(|le| le.document);
	let mut bad = Vec::new();
	let mut sets: Vec<(String, Vec<String>)> = Vec::new();
	for f in d.fields() {
		if !names_as_typed && v2_escapes(&f.name) != f.name {
			bad.push(carry_diag(
				old.line(&f.path),
				"shcl 2.0 read this name with its backslash escapes resolved, and 3.0 reads it as typed; rename it and upgrade again",
			));
			continue;
		}
		if d.read_raw(&f.path).status == shcl::Status::Good {
			continue;
		}
		let arr = d.read_string_array(&f.path);
		if arr.status != shcl::Status::Good {
			continue;
		}
		let want: Vec<String> = arr.value.iter().map(|v| v2_escapes(v)).collect();
		if want == arr.value {
			continue;
		}
		let s = d.read_string(&f.path);
		let st = if want.len() == 1 && s.status == shcl::Status::Good && s.value == arr.value[0] {
			d.set_string(&f.path, &want[0])
		} else {
			let refs: Vec<&str> = want.iter().map(String::as_str).collect();
			d.set_string_array(&f.path, &refs)
		};
		if st != shcl::SetStatus::Ok {
			bad.push(carry_diag(
				old.line(&f.path),
				"the upgraded file can't hold this value as shcl 2.0 read it",
			));
			continue;
		}
		sets.push((f.path, want));
	}
	let text = if sets.is_empty() {
		up.to_owned()
	} else {
		d.to_text_keep_lines().0
	};
	let now = shcl::Document::parse(&text);
	for (path, want) in &sets {
		let got = now.read_string_array(path);
		if got.status != shcl::Status::Good || got.value != *want {
			bad.push(carry_diag(
				old.line(path),
				"the upgraded file doesn't read this value as shcl 2.0 read it",
			));
		}
	}
	for p in old.paths() {
		if old.count(&p) != 1 {
			continue;
		}
		let (was, is) = (old.read_string_array(&p), now.read_string_array(&p));
		if was.status == shcl2::Status::Good && is.status == shcl::Status::Good && was.value != is.value {
			bad.push(carry_diag(
				old.line(&p),
				"the upgraded file doesn't read this value as shcl 2.0 read it",
			));
		}
	}
	if bad.is_empty() {
		return Ok(text);
	}
	let mut lines: Vec<usize> = bad.iter().map(|d| d.line).collect();
	lines.sort_unstable();
	lines.dedup();
	let mut e = Error::new(ErrKind::Refused).msg(format!(
		"{} lines don't read as shcl 2.0 read them; fix them and upgrade again",
		lines.len()
	));
	e.diags = bad;
	Err(e)
}

fn carry_diag(line: usize, message: &str) -> Diag {
	Diag {
		line,
		severity: Severity::Error,
		code: "W002",
		message: message.to_owned(),
		layer: 0,
	}
}

/// `s` as shcl 1.2 and 2.0 read it: `\t`, `\n`, `\\`, `\"`, and `\'` are
/// escapes, and any other backslash is itself.
fn v2_escapes(s: &str) -> String {
	let mut out = String::with_capacity(s.len());
	let mut chars = s.chars();
	while let Some(c) = chars.next() {
		if c != '\\' {
			out.push(c);
			continue;
		}
		match chars.next() {
			None => out.push('\\'),
			Some('t') => out.push('\t'),
			Some('n') => out.push('\n'),
			Some(e @ ('\\' | '"' | '\'')) => out.push(e),
			Some(other) => {
				out.push('\\');
				out.push(other);
			}
		}
	}
	out
}

fn upgraded(u: shcl::Upgrade) -> Upgraded {
	Upgraded {
		text: u.text,
		from: u.format,
		changed: !u.current,
		unsure: u.lost,
		backup: u.backup,
	}
}

/// `text` as a save writes it. Where that has no Format line, 2.x's block is
/// swapped for the current one where it sits, as the upgrade does, so the
/// next `open` doesn't take the file as 2.x (2026101513330010).
fn saved(text: &str, info: crate::Info, stamp: bool) -> Result<String, Error> {
	let e = info_edit();
	let out = crate::finish_info(text, info, stamp, &e)?;
	if format_of(&out) != 0 {
		return Ok(out);
	}
	let (marked, found) = crate::info::old_block(&out, &e);
	if !found {
		return Ok(out);
	}
	crate::info::place_block(&marked, marked != out, &e)
}

fn info_edit() -> crate::InfoEdit<'static> {
	crate::InfoEdit {
		block: shcl::GEN_BANNER,
		major: FORMAT_MAJOR,
		values,
	}
}

/// Every field's path and value and the load's error codes, for
/// `crate::finish_info` to tell a comment from a raw body's line.
fn values(text: &str) -> String {
	let d = shcl::Document::parse(text);
	let mut s = String::new();
	for f in d.fields() {
		s.push_str(&f.path);
		s.push('\0');
		s.push_str(&f.value);
		s.push('\x01');
	}
	for x in d.diagnostics() {
		if x.severity == shcl::Severity::Error {
			s.push_str(x.code);
			s.push('\x02');
		}
	}
	s
}

impl Doc {
	/// Builds the reads' document from the layers again. Lower layers never
	/// change, but a merge changes its base, so it starts over.
	fn remerge(&mut self) {
		let Some(lay) = &self.lay else { return };
		let mut m = shcl::Document::new();
		for x in lay.docs.iter().flatten() {
			m.merge(&x.d);
		}
		self.d = m;
	}

	/// Where edits and saves go: the edit layer of a layered document, or
	/// the document itself.
	fn top(&self) -> &Doc {
		match &self.lay {
			Some(l) => l.docs[l.edit].as_ref().unwrap_or(self),
			None => self,
		}
	}

	/// Runs an edit on the edit layer, then builds the merge again.
	fn on_top<T>(&mut self, edit: impl FnOnce(&mut Doc) -> T) -> T {
		let Some(lay) = &mut self.lay else { return edit(self) };
		let out = match &mut lay.docs[lay.edit] {
			Some(top) => edit(top),
			// open_layers always fills the edit layer.
			None => unreachable!("no edit layer"),
		};
		self.remerge();
		out
	}

	/// The save's text before the info block and the Format line.
	fn lines(&self) -> String {
		match self.d.to_text_keep_lines() {
			(t, true) => t,
			(_, false) => self.d.to_canonical(),
		}
	}

	/// `text` with the info block and the Format line as `info_block` and
	/// `stamp_format` asked, or as it is when an edit since made them
	/// impossible, which `write` reports.
	fn finish(&self, text: String) -> String {
		saved(&text, self.info, self.stamp).unwrap_or(text)
	}

	/// The shcl document underneath, for calls shclw doesn't cover. On a
	/// layered document it's the merge, which `open_layers` builds again
	/// after every edit.
	pub fn inner(&self) -> &shcl::Document {
		&self.d
	}

	/// The shcl document underneath, to change it with calls shclw doesn't
	/// cover. On a layered document that's the merge, so a change there is
	/// gone after the next edit, and never saved.
	pub fn inner_mut(&mut self) -> &mut shcl::Document {
		&mut self.d
	}

	fn set_err(path: &str, st: shcl::SetStatus) -> Result<(), Error> {
		use shcl::SetStatus as S;
		let kind = match st {
			S::Ok => return Ok(()),
			S::BadPath
			| S::ValueInPath
			| S::Wildcard
			| S::NoSuchIndex
			| S::TooDeep
			| S::Multiple
			| S::UnderArray
			| S::HasChildren
			| S::RawBlock => ErrKind::BadPath,
			S::NotFinite
			| S::BadDateTime
			| S::BadRawInfo
			| S::BadRawBody
			| S::BadComment
			| S::NotOneValue
			| S::NotUtf8
			| S::OutOfRange
			| S::NoReadBack => ErrKind::BadValue,
		};
		Err(Error::new(kind).path(path).msg(format!("{st:?}")))
	}
}

impl ShclDoc for Doc {
	fn text(&self) -> String {
		let t = self.top();
		t.finish(t.lines())
	}

	fn canonical(&self) -> String {
		let t = self.top();
		t.finish(t.d.to_canonical())
	}

	/// Refuses when the load dropped lines the save would lose, the same
	/// check shcl's `save_file_keep_lines` makes, and writes nothing when the
	/// info block or the Format line can't go in, since an edit made after
	/// `info_block` or `stamp_format`. A save with no Format line has 2.x's
	/// info block swapped for the current one where it sat. A write over
	/// the file `open` read leaves it alone with `Changed` when it changed
	/// on disk since, unless `force`.
	fn write_with(&mut self, path: &str, opts: &WriteOpts) -> Result<(), Error> {
		if let Some(lay) = &mut self.lay {
			if let Some(top) = &mut lay.docs[lay.edit] {
				return top.write_with(path, opts);
			}
		}
		if self.newer > 0 {
			return Err(too_new(self.newer).path(path));
		}
		let (text, kept) = self.d.to_text_keep_lines();
		let lost = self.d.lost_count();
		if !kept && lost > 0 {
			return Err(Error::new(ErrKind::Refused)
				.path(path)
				.msg(format!("{lost} lines would be lost")));
		}
		let text = if kept { text } else { self.d.to_canonical() };
		let text = saved(&text, self.info, self.stamp).map_err(|e| e.path(path))?;
		let Some(f) = self.file.as_mut().filter(|f| f.over(path)) else {
			return write_atomic(path, &text);
		};
		match self.up.as_mut().filter(|u| u.backup.is_empty()) {
			Some(up) => {
				up.backup = f.replace(path, &text, || keep_original(path, &f.text, up.from), opts.force)?;
			}
			None => {
				f.replace(path, &text, || Ok(String::new()), opts.force)?;
			}
		}
		f.text = text;
		f.gone = false;
		f.id = file_id(path);
		Ok(())
	}

	fn read_only(&self) -> bool {
		self.top().newer > 0
	}

	/// 3.0 writes the canonical form when it can't keep the lines with the
	/// edits in, such as after a child is added under a flat dotted line,
	/// and comments can move. This builds the save's text to find out.
	/// `info_block` and `stamp_format` change only their own lines, so they
	/// don't change the answer.
	fn rewrites(&self) -> bool {
		let s = self.top();
		if s.up.as_ref().is_some_and(|u| u.backup.is_empty()) {
			return true;
		}
		!s.d.to_text_keep_lines().1
	}

	/// Reads, `lines`, `diags`, and `text` are all of the upgraded text.
	fn upgraded_from(&self) -> u32 {
		self.top().up.as_ref().map_or(0, |u| u.from)
	}

	fn backup_name(&self) -> &str {
		self.top().up.as_ref().map_or("", |u| u.backup.as_str())
	}

	/// A layered document has every layer's, in layer order, each with its
	/// `layer`.
	fn diags(&self) -> Vec<Diag> {
		let Some(lay) = &self.lay else {
			return diags(self.d.diagnostics());
		};
		let mut out = Vec::new();
		for (i, x) in lay.docs.iter().enumerate() {
			let Some(x) = x else { continue };
			out.extend(x.diags().into_iter().map(|g| Diag { layer: i + 1, ..g }));
		}
		out
	}

	/// On a layered document it's each layer's own, and not what the
	/// layers' merge folded.
	fn merged(&self) -> Vec<Merge> {
		let Some(lay) = &self.lay else {
			return crate::merges_of(&self.src, parse, 0);
		};
		let mut out = Vec::new();
		for (i, x) in lay.docs.iter().enumerate() {
			if let Some(x) = x {
				out.extend(crate::merges_of(&x.src, parse, i + 1));
			}
		}
		out
	}

	fn errors(&self) -> usize {
		match &self.lay {
			Some(lay) => lay.docs.iter().flatten().map(ShclDoc::errors).sum(),
			None => self.d.error_count(),
		}
	}

	fn check(&self, schema: &Doc) -> Vec<Diag> {
		diags(&self.d.validate(&schema.d))
	}

	fn drop_declared(&self, ds: Vec<Diag>) -> Vec<Diag> {
		let mut v = undiags(&ds);
		shcl::suppress_declared_repeats(&self.d, &mut v);
		shcl::suppress_declared_reopens(&self.d, &mut v);
		diags(&v)
	}

	fn int(&self, path: &str, fallback: i64) -> i64 {
		self.d.get_int_or(path, fallback)
	}

	fn float(&self, path: &str, fallback: f64) -> f64 {
		// Through get_float, so infinity gets the fallback too.
		let g = self.get_float(path);
		if g.ok() {
			g.value
		} else {
			fallback
		}
	}

	fn bool(&self, path: &str, fallback: bool) -> bool {
		self.d.get_bool_or(path, fallback)
	}

	fn str(&self, path: &str, fallback: &str) -> String {
		self.d.get_string_or(path, fallback.to_owned())
	}

	fn strs(&self, path: &str, fallback: &[&str]) -> Vec<String> {
		self.d
			.get_string_array_or(path, fallback.iter().map(|s| (*s).to_owned()).collect())
	}

	fn ints(&self, path: &str, fallback: &[i64]) -> Vec<i64> {
		self.d.get_int_array_or(path, fallback.to_vec())
	}

	fn floats(&self, path: &str, fallback: &[f64]) -> Vec<f64> {
		let g = self.get_floats(path);
		if g.ok() {
			g.value
		} else {
			fallback.to_vec()
		}
	}

	fn raw(&self, path: &str, fallback: &str) -> String {
		self.d.get_raw_or(path, fallback.to_owned())
	}

	fn get_int(&self, path: &str) -> Got<i64> {
		got(self.d.read_int(path))
	}

	fn get_float(&self, path: &str) -> Got<f64> {
		crate::finite_float(got(self.d.read_float(path)))
	}

	fn get_bool(&self, path: &str) -> Got<bool> {
		got(self.d.read_bool(path))
	}

	fn get_str(&self, path: &str) -> Got<String> {
		got(self.d.read_string(path))
	}

	fn get_strs(&self, path: &str) -> Got<Vec<String>> {
		got(self.d.read_string_array(path))
	}

	fn get_ints(&self, path: &str) -> Got<Vec<i64>> {
		got(self.d.read_int_array(path))
	}

	fn get_floats(&self, path: &str) -> Got<Vec<f64>> {
		crate::finite_floats(got(self.d.read_float_array(path)))
	}

	fn get_raw(&self, path: &str) -> Got<String> {
		got(self.d.read_raw(path))
	}

	fn has(&self, path: &str) -> bool {
		self.d.exists(path)
	}

	fn count(&self, path: &str) -> usize {
		self.d.count(path)
	}

	fn names(&self, path: &str) -> Vec<String> {
		crate::dedupe_names(self.d.children(path))
	}

	/// 3.0's own instance paths, with an index on every step that repeats.
	/// A wildcard slot that reaches no node or several has none, and is left
	/// out.
	fn each(&self, path: &str) -> Vec<Node> {
		self.d
			.read_fields(path)
			.value
			.into_iter()
			.filter(|f| !f.path.is_empty())
			.map(|f| Node {
				path: f.path,
				value: f.value,
				line: f.line,
			})
			.collect()
	}

	fn line(&self, path: &str) -> usize {
		self.d.line(path)
	}

	fn child(&self, base: &str, name: &str) -> String {
		if base.is_empty() {
			return shcl::quote_segment(name);
		}
		format!("{base}.{}", shcl::quote_segment(name))
	}

	fn put(&mut self, path: &str, v: impl Into<Value>) -> Result<(), Error> {
		let v = v.into();
		if self.lay.is_some() {
			return self.on_top(|t| t.put(path, v));
		}
		crate::check_value(path, &v)?;
		let st = match &v {
			Value::Int(x) => self.d.set_int(path, *x),
			Value::Float(x) => self.d.set_float(path, *x),
			Value::Bool(x) => self.d.set_bool(path, *x),
			Value::Str(x) => self.d.set_string(path, x),
			Value::Strs(x) => {
				let refs: Vec<&str> = x.iter().map(String::as_str).collect();
				self.d.set_string_array(path, &refs)
			}
			Value::Ints(x) => self.d.set_int_array(path, x),
			Value::Floats(x) => self.d.set_float_array(path, x),
		};
		Doc::set_err(path, st)?;
		crate::verify_put(self, path, &v)
	}

	fn put_syntax(&mut self, path: &str, text: &str) -> Result<(), Error> {
		if self.lay.is_some() {
			return self.on_top(|t| t.put_syntax(path, text));
		}
		Doc::set_err(path, self.d.set_literal(path, text))?;
		let mut alone = new();
		// A failed set leaves alone empty, which the check reports.
		let _ = alone.d.set_literal("k", text);
		crate::verify_syntax(self, path, &alone, &parse(&format!("k: {text}\n")))
	}

	fn comment(&mut self, path: &str, text: &str) -> Result<(), Error> {
		self.on_top(|t| Doc::set_err(path, t.d.set_comment(path, text)))
	}

	fn remove(&mut self, path: &str) -> usize {
		self.on_top(|t| t.d.remove(path))
	}

	/// shcl's own `set_banner` leaves an old block above the first field.
	fn info_block(&mut self, on: bool) -> Result<(), Error> {
		if self.lay.is_some() {
			return self.on_top(|t| t.info_block(on));
		}
		let info = if on { crate::Info::On } else { crate::Info::Off };
		crate::finish_info(&self.lines(), info, self.stamp, &info_edit())?;
		self.info = info;
		Ok(())
	}

	fn stamp_format(&mut self) -> Result<(), Error> {
		if self.lay.is_some() {
			return self.on_top(ShclDoc::stamp_format);
		}
		crate::finish_info(&self.lines(), self.info, true, &info_edit())?;
		self.stamp = true;
		Ok(())
	}
}

fn too_new(f: u32) -> Error {
	Error::new(ErrKind::TooNew).msg(format!(
		"format {f}, and shcl {SHCL_VERSION} reads up to {FORMAT_MAJOR}"
	))
}

fn strictness(s: Strictness) -> shcl::Strictness {
	match s {
		Strictness::Loose => shcl::Strictness::Loose,
		Strictness::Standard => shcl::Strictness::Standard,
		Strictness::Strict => shcl::Strictness::Strict,
	}
}

fn status(s: shcl::Status) -> Status {
	match s {
		shcl::Status::Good => Status::Good,
		shcl::Status::Empty => Status::Empty,
		shcl::Status::NotFound => Status::NotFound,
		shcl::Status::BadType => Status::BadType,
		shcl::Status::Multiple => Status::Multiple,
		shcl::Status::BadPath => Status::BadPath,
	}
}

fn got<T>(r: shcl::Read<T>) -> Got<T> {
	Got {
		value: r.value,
		status: status(r.status),
		line: r.line,
		text: r.raw.unwrap_or_default(),
	}
}

fn diags(v: &[shcl::Diagnostic]) -> Vec<Diag> {
	v.iter()
		.map(|d| Diag {
			line: d.line,
			severity: match d.severity {
				shcl::Severity::Error => Severity::Error,
				shcl::Severity::Hint => Severity::Hint,
			},
			code: d.code,
			message: d.message.clone(),
			layer: 0,
		})
		.collect()
}

fn undiags(v: &[Diag]) -> Vec<shcl::Diagnostic> {
	v.iter()
		.map(|d| shcl::Diagnostic {
			line: d.line,
			severity: match d.severity {
				Severity::Error => shcl::Severity::Error,
				Severity::Hint => shcl::Severity::Hint,
			},
			code: d.code,
			message: d.message.clone(),
		})
		.collect()
}
