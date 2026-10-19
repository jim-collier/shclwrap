// Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
// SPDX-License-Identifier: MIT

//! The info block and the Format line, the same for every module. Go's
//! `shclw/info.go` is the twin.

use crate::{ErrKind, Error};

const INFO_TITLE: &str = " This config file format is SHCL.";
const INFO_NAME: &str = " \"Simple Hierarchical Config Language\"";
const FORMAT_WORD: &str = "##    Format";
const MIGRATED_LINE: &str = "##    Migrated from SHCL 2.x.";
const SCHEMA_HEAD: &str = "##    Schema   ";

/// What a save does with the info block.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Info {
	#[default]
	AsIs,
	On,
	Off,
}

/// What a module hands `finish_info`. `values` sums up the values and errors
/// text loads to with the bound shcl. A run of lines whose removal leaves
/// that the same is comments, which is how a block's text inside a raw body
/// is told from a real block.
#[derive(Debug, Clone, Copy)]
pub struct InfoEdit<'a> {
	pub block: &'a str,
	pub major: u32,
	pub values: fn(&str) -> String,
}

/// `text` as a save writes it after `info_block` and `stamp_format`
/// (design.md, Format and upgrade).
pub fn finish_info(text: &str, info: Info, stamp: bool, e: &InfoEdit<'_>) -> Result<String, Error> {
	if info == Info::AsIs && !stamp {
		return Ok(text.to_owned());
	}
	let (bom, text) = match text.strip_prefix('\u{feff}') {
		Some(rest) => ("\u{feff}", rest),
		None => ("", text),
	};
	let mut f = InfoLines::split(text);
	let base = (e.values)(text);
	let n = f.lines.len();
	let comments = |f: &InfoLines, drop: &[bool]| (e.values)(&f.join_without(Some(drop))) == base;

	let mut named = false;
	for v in formats_named(&f, e, &base) {
		if v > e.major {
			return Err(Error::new(ErrKind::TooNew).msg(format!("the file names format {v}")));
		}
		if v < e.major {
			return Err(Error::new(ErrKind::Refused).msg(format!("the file names format {v}; upgrade it first")));
		}
		named = true;
	}

	if info != Info::AsIs {
		named = false;
		let mut drop = vec![false; n];
		for (start, end) in info_runs(&f) {
			// A lone stamp the save would add back stays where it is.
			if stamp && info == Info::Off && format_number(f.trimmed(start), e.major) == Some(e.major) {
				named = true;
				continue;
			}
			let mut run = vec![false; n];
			for (i, r) in run.iter_mut().enumerate().take(end).skip(start) {
				// shcl keeps a Schema line that sits in the block.
				*r = !f.trimmed(i).starts_with(SCHEMA_HEAD);
			}
			if comments(&f, &run) {
				for (d, r) in drop.iter_mut().zip(&run) {
					*d = *d || *r;
				}
			}
		}
		f.remove(&drop);
	}
	let before = f.join_without(None);
	if info == Info::On {
		f.trim_blank_tail();
		if !f.lines.is_empty() {
			f.add(String::new());
		}
		for l in e.block.trim_end_matches('\n').split('\n') {
			f.add(l.to_owned());
		}
		named = true;
	}
	if stamp && !named {
		f.add(format!("{FORMAT_WORD}   {}", e.major));
	}
	let out = f.join_without(None);
	if out != before && (e.values)(&out) != (e.values)(&before) {
		return Err(Error::new(ErrKind::Refused).msg("the file ends in a raw block that never closes"));
	}
	Ok(format!("{bom}{out}"))
}

/// Stands in for 2.x's block while shcl upgrades the text, so the new block
/// can go where the old one was.
const INFO_MARK: &str = "# shclw: the info block goes here";

/// Whether `text` has 2.x's `#` info block as comments, and `text` with the
/// first one swapped for a mark `place_block` finds. Where `text` already has
/// a line like the mark, the text comes back as it was.
pub(crate) fn old_block(text: &str, e: &InfoEdit<'_>) -> (String, bool) {
	let body = text.strip_prefix('\u{feff}').unwrap_or(text);
	let mut f = InfoLines::split(body);
	let base = (e.values)(body);
	let n = f.lines.len();
	let title = format!("#{INFO_TITLE}");
	for (start, end) in info_runs(&f) {
		if f.trimmed(start) != title && (start + 1 == end || f.trimmed(start + 1) != title) {
			continue;
		}
		let mut run = span(n, start, end);
		let mut last = start;
		for (i, r) in run.iter_mut().enumerate().take(end).skip(start) {
			*r = !f.trimmed(i).starts_with(SCHEMA_HEAD);
			if *r {
				last = i;
			}
		}
		if (e.values)(&f.join_without(Some(&run))) != base {
			continue;
		}
		if (0..n).any(|i| f.trimmed(i) == INFO_MARK) {
			return (text.to_owned(), true);
		}
		INFO_MARK.clone_into(&mut f.lines[last]);
		run[last] = false;
		return (
			format!("{}{}", &text[..text.len() - body.len()], f.join_without(Some(&run))),
			true,
		);
	}
	(text.to_owned(), false)
}

/// `text`, an upgrade of `old_block`'s marked text, with every old block off
/// and the info block where the mark is. With no mark it goes at the end, as
/// `info_block` puts it.
pub(crate) fn place_block(text: &str, marked: bool, e: &InfoEdit<'_>) -> Result<String, Error> {
	let off = finish_info(text, Info::Off, false, e)?;
	let body = off.strip_prefix('\u{feff}').unwrap_or(&off);
	let bom = &off[..off.len() - body.len()];
	let f = InfoLines::split(body);
	let marks: Vec<bool> = (0..f.lines.len()).map(|i| f.trimmed(i) == INFO_MARK).collect();
	let at = marks.iter().position(|&m| m);
	let Some(at) = at.filter(|_| marked && body.matches(INFO_MARK).count() == 1) else {
		let off = if marked {
			format!("{bom}{}", f.join_without(Some(&marks)))
		} else {
			off.clone()
		};
		return finish_info(&off, Info::On, false, e);
	};
	let mut g = InfoLines {
		lines: Vec::with_capacity(f.lines.len() + 8),
		ends: Vec::with_capacity(f.lines.len() + 8),
		eol: f.eol,
	};
	for (i, l) in f.lines.iter().enumerate() {
		if i != at {
			g.lines.push(l.clone());
			g.ends.push(f.ends[i]);
			continue;
		}
		let block: Vec<&str> = e.block.trim_end_matches('\n').split('\n').collect();
		for (k, b) in block.iter().enumerate() {
			g.lines.push((*b).to_owned());
			g.ends.push(if k + 1 == block.len() { f.ends[i] } else { f.eol });
		}
	}
	let out = g.join_without(None);
	if (e.values)(&out) != (e.values)(body) {
		return Err(Error::new(ErrKind::Refused).msg("the file ends in a raw block that never closes"));
	}
	Ok(format!("{bom}{out}"))
}

/// `text`, an upgrade of `orig`, with `orig`'s line ends, where shcl wrote it
/// out again with LF only.
pub(crate) fn same_ends(orig: &str, text: &str, e: &InfoEdit<'_>) -> String {
	if InfoLines::split(orig).eol != "\r\n" || text.contains('\r') {
		return text.to_owned();
	}
	let crlf = text.replace('\n', "\r\n");
	if (e.values)(&crlf) != (e.values)(text) {
		return text.to_owned();
	}
	crlf
}

/// The number on each Format line that counts, in order. A line counts when
/// taking it out leaves the values at `base`, so one in a raw body doesn't.
fn formats_named(f: &InfoLines, e: &InfoEdit<'_>, base: &str) -> Vec<u32> {
	let n = f.lines.len();
	(0..n)
		.filter_map(|i| format_number(f.trimmed(i), e.major).map(|v| (i, v)))
		.filter(|&(i, v)| v != 0 && (e.values)(&f.join_without(Some(&span(n, i, i + 1)))) == base)
		.map(|(_, v)| v)
		.collect()
}

/// Reads a Format line as shcl 3.0 does: the head, then digits only. Some
/// for any line with the head, which comes off as a stamp, with 0 when the
/// number doesn't read. Digits too big read as `major`, as shcl reads them.
fn format_number(trimmed: &str, major: u32) -> Option<u32> {
	if trimmed == FORMAT_WORD {
		return Some(0);
	}
	let digits = trimmed.strip_prefix(FORMAT_WORD)?.strip_prefix("   ")?;
	if digits.is_empty() || !digits.bytes().all(|b| b.is_ascii_digit()) {
		return Some(0);
	}
	Some(digits.parse().unwrap_or(major))
}

/// Each info block and lone Format line, as `(start, end)`. A block runs
/// from the lone mark above its title line to the next lone mark, or, with
/// none, through the lines written the block's way. That's shcl 3.0's rule
/// for its own setter, with 2.x's `#` block found the same way. A Format
/// line outside a block comes with the migrated note under it.
fn info_runs(text: &InfoLines) -> Vec<(usize, usize)> {
	let mut runs: Vec<(usize, usize)> = Vec::new();
	let count = text.lines.len();
	let mut i = 0;
	while i < count {
		let line = text.trimmed(i);
		let Some(mark) = line.strip_suffix(INFO_TITLE).filter(|m| *m == "##" || *m == "#") else {
			let mut end = i + 1;
			if format_number(line, 0).is_some() {
				if end < count && text.trimmed(end) == MIGRATED_LINE {
					end += 1;
				}
				runs.push((i, end));
			}
			i = end;
			continue;
		};
		let mut start = i;
		let mut end = i + 1;
		if i > 0 && text.trimmed(i - 1) == mark && runs.last().is_none_or(|r| r.1 < i) {
			start = i - 1;
		}
		let mut closed = false;
		let mut next = end;
		while next < count && text.trimmed(next).starts_with(mark) {
			if text.trimmed(next) == mark {
				end = next + 1;
				closed = true;
				break;
			}
			next += 1;
		}
		while !closed
			&& end < count
			&& text
				.trimmed(end)
				.strip_prefix(mark)
				.is_some_and(|rest| rest.starts_with("    ") || rest == INFO_NAME)
		{
			end += 1;
		}
		runs.push((start, end));
		i = end;
	}
	runs
}

fn span(n: usize, start: usize, end: usize) -> Vec<bool> {
	(0..n).map(|i| i >= start && i < end).collect()
}

/// Text as lines, each with its own line end, so a CRLF file keeps them. An
/// added line ends the way most lines in the file do, LF on a tie, as
/// shcl's stamp does.
struct InfoLines {
	lines: Vec<String>,
	ends: Vec<&'static str>,
	eol: &'static str,
}

impl InfoLines {
	fn split(text: &str) -> InfoLines {
		let mut f = InfoLines {
			lines: Vec::new(),
			ends: Vec::new(),
			eol: "\n",
		};
		let mut crlf = 0;
		let mut rest = text;
		while !rest.is_empty() {
			let (line, end) = match rest.find('\n') {
				Some(i) if rest[..i].ends_with('\r') => {
					crlf += 1;
					(&rest[..i - 1], "\r\n")
				}
				Some(i) => (&rest[..i], "\n"),
				None => (rest, ""),
			};
			f.lines.push(line.to_owned());
			f.ends.push(end);
			rest = &rest[line.len() + end.len()..];
		}
		if crlf * 2 > f.lines.len() {
			f.eol = "\r\n";
		}
		f
	}

	/// Line `i` without its indent or trailing blanks. Only ASCII ones, so Go
	/// and Rust agree.
	fn trimmed(&self, i: usize) -> &str {
		self.lines[i].trim_matches([' ', '\t', '\r'])
	}

	fn join_without(&self, drop: Option<&[bool]>) -> String {
		let mut s = String::new();
		for (i, l) in self.lines.iter().enumerate() {
			if drop.is_none_or(|d| !d[i]) {
				s.push_str(l);
				s.push_str(self.ends[i]);
			}
		}
		s
	}

	fn add(&mut self, line: String) {
		if let Some(last) = self.ends.last_mut() {
			if last.is_empty() {
				*last = self.eol;
			}
		}
		self.lines.push(line);
		self.ends.push(self.eol);
	}

	/// Takes out the dropped lines. Where a run went, the blank lines around
	/// it become one, or none at the top or the end of the file, so what was
	/// set apart stays apart.
	fn remove(&mut self, drop: &[bool]) {
		let n = self.lines.len();
		let mut lines = Vec::with_capacity(n);
		let mut ends = Vec::with_capacity(n);
		let mut i = 0;
		while i < n {
			if !drop[i] && !blank(&self.lines[i]) {
				lines.push(self.lines[i].clone());
				ends.push(self.ends[i]);
				i += 1;
				continue;
			}
			// A stretch of blank and dropped lines.
			let mut k = i;
			let mut gone = false;
			let mut first_blank = None;
			while k < n && (drop[k] || blank(&self.lines[k])) {
				gone = gone || drop[k];
				if !drop[k] && first_blank.is_none() {
					first_blank = Some(k);
				}
				k += 1;
			}
			if !gone {
				lines.extend_from_slice(&self.lines[i..k]);
				ends.extend_from_slice(&self.ends[i..k]);
			} else if let Some(b) = first_blank.filter(|_| !lines.is_empty() && k < n) {
				lines.push(self.lines[b].clone());
				ends.push(self.ends[b]);
			}
			i = k;
		}
		self.lines = lines;
		self.ends = ends;
	}

	fn trim_blank_tail(&mut self) {
		while self.lines.last().is_some_and(|l| blank(l)) {
			self.lines.pop();
			self.ends.pop();
		}
	}
}

fn blank(l: &str) -> bool {
	l.trim_matches([' ', '\t', '\r']).is_empty()
}
