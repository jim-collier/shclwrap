# Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
# SPDX-License-Identifier: MIT

"""shclw for Python: the wrapper over shcl 3.0.

The same calls as Go's shclw3 and Rust's shclw::v3, with Python names. shcl
is found at import time: a shcl.py beside this file, else the installed one,
and anything but 3.0 is refused. shcl 2.0, for the upgrade check, is the copy
beside this file, _shclw_shcl2.py. See project/design.md.
"""

from __future__ import annotations

import errno
import importlib.util
import itertools
import math
import os
import re
import stat
import sys
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field
from decimal import Decimal
from enum import Enum
from pathlib import Path
from typing import Any, Generic, TypeVar

FORMAT_MAJOR = 3
"""The newest file format this shcl reads."""


def _load_beside(name: str, file: str) -> Any:
	"""A module from a file beside this one, under a name of its own, so it
	can't stand in for a shcl the rest of the program imports."""
	if name in sys.modules:
		return sys.modules[name]
	path = Path(__file__).with_name(file)
	spec = importlib.util.spec_from_file_location(name, path)
	if not path.is_file() or spec is None or spec.loader is None:
		return None
	mod = importlib.util.module_from_spec(spec)
	sys.modules[name] = mod
	try:
		spec.loader.exec_module(mod)
	except BaseException:
		del sys.modules[name]
		raise
	return mod


def _find_shcl() -> Any:
	# A shcl.py copied beside this file wins, since a program that bundles one
	# meant it (2026101513330050). Otherwise it's the installed one.
	mod = _load_beside("_shclw_shcl3", "shcl.py")
	if mod is None:
		try:
			import shcl as mod
		except ImportError as e:
			raise ImportError("shclw needs shcl 3.0's shcl.py, installed or copied beside shclw.py") from e
	# Before 3.0 no binding had FORMAT_MAJOR, so its absence is a 1.2 or a 2.0.
	found = getattr(mod, "FORMAT_MAJOR", None)
	if found != FORMAT_MAJOR:
		which = "older than 3.0" if found is None else f"format {found}"
		raise ImportError(
			f"shclw needs shcl 3.0, and the shcl at {getattr(mod, '__file__', '?')} is {which}; "
			"install 3.0, or copy its shcl.py beside shclw.py"
		)
	return mod


shcl = _find_shcl()
# 2.0 is for the upgrade check, and is shcl too, so it has a name of its own.
shcl2 = _load_beside("_shclw_shcl2", "_shclw_shcl2.py")
if shcl2 is None:
	raise ImportError("shclw: _shclw_shcl2.py, shcl 2.0 for the upgrade check, isn't beside shclw.py")

T = TypeVar("T")


class Status(Enum):
	"""How a read went."""

	Good = 0
	Empty = 1
	NotFound = 2
	BadType = 3
	Multiple = 4
	BadPath = 5


class Strictness(Enum):
	"""How forgiving a load is. Only Strict can fail one."""

	Loose = 0
	Standard = 1
	Strict = 2


class Severity(Enum):
	Error = "error"
	Hint = "hint"


@dataclass
class Diag:
	"""One diagnostic from a load or a schema check. line is 1-based. layer is
	the 1-based place in open_layers' list, or 0 for a document that isn't
	layered."""

	line: int
	severity: Severity
	code: str
	message: str
	layer: int = 0

	def __str__(self) -> str:
		if self.layer > 0:
			return f"layer {self.layer} line {self.line}: {self.code} {self.message}"
		return f"line {self.line}: {self.code} {self.message}"


@dataclass(frozen=True, order=True)
class Spot:
	"""A line in a loaded document. layer is as in Diag."""

	layer: int
	line: int


@dataclass
class Merge:
	"""A name shcl kept one copy of, since a later copy had the same name and
	value and was folded into it. lines is the kept copy's line, then each
	later copy's. path reads the kept copy, as a Node's does, and layer is as
	in Diag."""

	path: str
	lines: list[int]
	layer: int = 0


@dataclass(frozen=True)
class Commented:
	"""A comment line that reads as one key: value once its # comes off, such
	as a setting left in the file at its default. path is what the line would
	set. default says the line ends in a ## Default note, which is the line a
	put of path fills in."""

	path: str
	value: str
	line: int
	default: bool = False


@dataclass(frozen=True)
class Layer:
	"""One layer for open_layers: a file, or with file empty, text the program
	has, such as its defaults or this run's command line."""

	file: str = ""
	text: str = ""


@dataclass
class Got(Generic[T]):
	"""A read with its status. Check status before trusting value: an array
	that reads as BadType still has its items, with 0 for the bad ones."""

	value: T
	status: Status
	line: int = 0
	text: str = ""

	def ok(self) -> bool:
		return self.status is Status.Good


@dataclass(frozen=True)
class Node:
	"""One instance from each(). path reads this instance and no other."""

	path: str
	value: str
	line: int


class ErrKind(Enum):
	NotFound = "notfound"
	IO = "io"
	Strict = "strict"
	Refused = "refused"
	BadPath = "badpath"
	BadValue = "badvalue"
	TooNew = "toonew"
	Unsupported = "unsupported"
	Ambiguous = "ambiguous"
	BackupTaken = "backuptaken"
	NoReadBack = "noreadback"
	Changed = "changed"
	Exists = "exists"


class Error(Exception):
	"""The one error the wrapper raises. diags is set for a failed strict load
	and a refused upgrade."""

	def __init__(self, kind: ErrKind, path: str = "", msg: str = "", diags: list[Diag] | None = None) -> None:
		self.kind = kind
		self.path = path
		self.msg = msg
		self.diags = diags if diags is not None else []
		super().__init__()

	def __str__(self) -> str:
		s = "shclw: " + self.kind.value
		if self.path:
			s += f" {self.path!r}"
		if self.msg:
			s += ": " + self.msg
		if self.__cause__ is not None:
			s += f": {self.__cause__}"
		return s


@dataclass
class Upgraded:
	"""What an upgrade made of a file. from_ is the format it was written for.
	unsure counts lines the upgrade couldn't convert for sure. lost lists the
	lines of the original, from 1, with a value the old shcl read that the
	upgraded file doesn't have. rewritten lists the rest of the original's
	lines that the upgraded text doesn't have as typed. A line that only moved
	isn't one."""

	text: str
	from_: int
	changed: bool = False
	unsure: int = 0
	backup: str = ""
	lost: list[int] = field(default_factory=list)
	rewritten: list[int] = field(default_factory=list)


def rewritten_lines(was: str, now: str, lost: Sequence[int]) -> list[int]:
	"""The lines of was, from 1, that now doesn't have as typed, leaving out
	the ones in lost. Lines are matched as a multiset, so a line that only
	moved isn't listed (design.md, Decisions)."""
	have: dict[str, int] = {}
	for line in _text_lines(now):
		have[line] = have.get(line, 0) + 1
	gone = set(lost)
	out: list[int] = []
	for i, line in enumerate(_text_lines(was), 1):
		if i in gone:
			continue
		if have.get(line, 0) > 0:
			have[line] -= 1
			continue
		out.append(i)
	return out


def _text_lines(s: str) -> list[str]:
	# Split on LF and keep any CR, so a line whose ending changed counts as
	# written differently.
	if not s:
		return []
	return s.removesuffix("\n").split("\n")


# Puts, as the few kinds shcl has setters for.
class _Kind(Enum):
	Int = 1
	Float = 2
	Bool = 3
	Str = 4
	Strs = 5
	Ints = 6
	Floats = 7


def _normalize(path: str, v: object) -> tuple[_Kind, Any]:
	# bool first, since it's an int too.
	if isinstance(v, bool):
		return _Kind.Bool, v
	if isinstance(v, int):
		return _Kind.Int, v
	if isinstance(v, float):
		return _finite(path, _Kind.Float, v)
	if isinstance(v, str):
		return _Kind.Str, v
	if isinstance(v, (list, tuple)):
		items = list(v)
		if all(isinstance(x, str) for x in items):
			return _Kind.Strs, items
		if not any(isinstance(x, bool) for x in items):
			if all(isinstance(x, int) for x in items):
				return _Kind.Ints, items
			if all(isinstance(x, (int, float)) for x in items):
				return _finite(path, _Kind.Floats, [float(x) for x in items])
	raise Error(ErrKind.BadValue, path, f"no setter for {type(v).__name__}")


def _finite(path: str, kind: _Kind, v: Any) -> tuple[_Kind, Any]:
	# No read takes infinity or NaN back (design.md, What the wrapper evens out).
	vals = v if isinstance(v, list) else [v]
	if not all(math.isfinite(x) for x in vals):
		raise Error(ErrKind.BadValue, path, "not a finite number")
	return kind, v


def _finite_float(g: Got[float]) -> Got[float]:
	if g.status is Status.Good and not math.isfinite(g.value):
		g.status, g.value = Status.BadType, 0.0
	return g


def _finite_floats(g: Got[list[float]]) -> Got[list[float]]:
	if g.status is not Status.Good or all(math.isfinite(x) for x in g.value):
		return g
	g.value = [x if math.isfinite(x) else 0.0 for x in g.value]
	g.status = Status.BadType
	return g


def _ascii_lower(s: str) -> str:
	return s.translate(_ASCII_LOWER)


_ASCII_LOWER = {c: c + 32 for c in range(ord("A"), ord("Z") + 1)}


def _dedupe_names(names: list[str]) -> list[str]:
	"""The first of each name, compared without ASCII case, as shcl matches them."""
	seen: set[str] = set()
	out: list[str] = []
	for n in names:
		k = _ascii_lower(n)
		if k not in seen:
			seen.add(k)
			out.append(n)
	return out


def read_text(path: str) -> str:
	"""A whole file, with a missing one as NotFound and one that isn't UTF-8 as IO."""
	try:
		b = Path(path).read_bytes()
	except FileNotFoundError as e:
		raise Error(ErrKind.NotFound, path) from e
	except OSError as e:
		raise Error(ErrKind.IO, path) from e
	try:
		return b.decode("utf-8")
	except UnicodeDecodeError:
		raise Error(ErrKind.IO, path, "not valid UTF-8") from None


def _changed(path: str) -> Error:
	return Error(ErrKind.Changed, path, "changed since it was read; nothing written")


def _exists(path: str) -> Error:
	return Error(ErrKind.Exists, path, "already there; nothing written")


def _check_mode(path: str, mode: int | None) -> None:
	if mode is not None and mode & ~0o777:
		raise Error(ErrKind.BadValue, path, f"mode {mode:#o} is more than permission bits")


_temp_seq = itertools.count()


def create_new(path: str, data: str, mode: int | None = None) -> None:
	"""Writes data to path only when nothing is there, and raises Exists
	otherwise. The text is synced under a temp name and hard-linked into
	place, so the name never holds part of it, and of 2 creates at once
	exactly one wins. A filesystem with no hard links gets an exclusive
	create of path itself (design.md, Loading and saving)."""
	_check_mode(path, mode)
	try:
		tmp = _write_temp(path, data, mode)
	except OSError as e:
		raise _create_err(path, e) from e
	try:
		os.link(tmp, path)
	except FileExistsError as e:
		raise _exists(path) from e
	except OSError as e:
		if not _no_links(e):
			raise _create_err(path, e) from e
		_create_in_place(path, data, mode)
		return
	finally:
		os.remove(tmp)
	_sync_dir(path)


def _no_links(e: OSError) -> bool:
	"""A link error that says the filesystem has no hard links. Linux gives
	EPERM, and Windows on FAT gives ERROR_INVALID_FUNCTION (1), which Python
	calls EINVAL, so the Windows codes go by winerror."""
	if isinstance(e, PermissionError) or e.errno in (errno.ENOTSUP, errno.EOPNOTSUPP, errno.ENOSYS):
		return True
	return getattr(e, "winerror", None) in (1, 50, 120)


def _write_temp(path: str, data: str, mode: int | None) -> str:
	head, base = os.path.split(path)
	# shcl's temp names cut the base the same way, to stay under 255 bytes.
	b = base.encode("utf-8", "surrogateescape")[:64].decode("utf-8", "ignore")
	last: OSError = FileExistsError(errno.EEXIST, "temp names taken")
	for _ in range(8):
		tmp = os.path.join(head, f".{b}.new{os.getpid()}.{next(_temp_seq)}")
		try:
			fd = _open_new(tmp, mode)
		except FileExistsError as e:
			last = e
			continue
		try:
			_fill(fd, data, mode)
		except OSError:
			os.remove(tmp)
			raise
		return tmp
	raise last


def _create_in_place(path: str, data: str, mode: int | None) -> None:
	"""create_new without a temp, for a filesystem with no hard links. A crash
	part way leaves part of the file."""
	try:
		fd = _open_new(path, mode)
	except FileExistsError as e:
		raise _exists(path) from e
	except OSError as e:
		raise _create_err(path, e) from e
	try:
		_fill(fd, data, mode)
	except OSError as e:
		os.remove(path)  # ours, made just above
		raise _create_err(path, e) from e
	_sync_dir(path)


def _open_new(path: str, mode: int | None) -> int:
	# One with a mode is born private, and gets its mode exactly in _fill,
	# past the umask.
	flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_BINARY", 0)
	return os.open(path, flags, 0o666 if mode is None else 0o600)


def _fill(fd: int, data: str, mode: int | None) -> None:
	with os.fdopen(fd, "wb") as f:
		f.write(data.encode("utf-8", "surrogateescape"))
		if mode is not None and os.name != "nt":
			os.fchmod(fd, mode)
		f.flush()
		os.fsync(fd)


def _sync_dir(path: str) -> None:
	if os.name == "nt":
		return
	try:
		fd = os.open(os.path.dirname(path) or ".", os.O_RDONLY)
	except OSError:
		return
	try:
		os.fsync(fd)
	except OSError:
		pass  # some filesystems can't; the file itself is synced
	finally:
		os.close(fd)


def _create_err(path: str, e: OSError) -> Error:
	return Error(ErrKind.NotFound if isinstance(e, FileNotFoundError) else ErrKind.IO, path, str(e))


def _write_mode(mode: int | None) -> Callable[[str, str], None]:
	"""_write_atomic where a file it creates gets mode. One that's there keeps
	its own, as shcl's atomic write keeps it."""
	if mode is None:
		return _write_atomic

	def write(path: str, text: str) -> None:
		if os.path.lexists(path):
			_write_atomic(path, text)
			return
		try:
			create_new(path, text, mode)
		except Error as e:
			if e.kind != ErrKind.Exists:
				raise
			_write_atomic(path, text)  # made in the meantime

	return write


def _unchanged(path: str, was: str) -> bool:
	try:
		return Path(path).read_bytes() == was.encode("utf-8")
	except OSError:
		return False


def _replace_read(
	path: str, was: str, data: str, backup: Callable[[], str] | None, write: Callable[[str, str], None]
) -> str:
	"""Go's ReplaceRead: data over path, unless path changed since it held was.
	backup keeps was first and names the copy, which comes back."""

	if not _unchanged(path, was):
		raise _changed(path)
	name = backup() if backup is not None else ""

	def drop() -> None:
		if name:
			Path(name).unlink(missing_ok=True)

	if not _unchanged(path, was):
		drop()
		raise _changed(path)
	try:
		write(path, data)
		return name
	except Error as e:
		err = e
	except OSError as e:
		err = Error(ErrKind.IO, path)
		err.__cause__ = e
	if _unchanged(path, was):
		drop()
		raise err
	if not name:
		raise err
	note = "the original is " + name
	if not os.path.lexists(path):
		try:
			os.rename(name, path)
			note = "the original was put back"
		except OSError:
			pass
	err.msg = f"{err.msg}; {note}" if err.msg else note
	raise err


def format_float(f: float) -> str:
	"""A float the same way in every wrapper language: the shortest digits that
	read back exactly, positional from 1e-5 to 1e17, else with an exponent."""
	if math.isnan(f):
		return "nan"
	if math.isinf(f):
		return "inf" if f > 0 else "-inf"
	neg = math.copysign(1.0, f) < 0
	if f == 0:
		digits, exp = "0", 0
	else:
		# repr is already the shortest round trip; only the layout differs.
		_, ds, e = Decimal(repr(abs(f))).as_tuple()
		digits = "".join(map(str, ds)).rstrip("0")
		exp = int(e) + len(ds) - 1
	sign = "-" if neg else ""
	if exp >= 17 or exp < -5:
		tail = "." + digits[1:] if len(digits) > 1 else ""
		return f"{sign}{digits[0]}{tail}e{exp}"
	if exp < 0:
		return f"{sign}0.{'0' * (-exp - 1)}{digits}"
	if len(digits) <= exp + 1:
		return f"{sign}{digits}{'0' * (exp + 1 - len(digits))}.0"
	return f"{sign}{digits[: exp + 1]}.{digits[exp + 1 :]}"


# Info block and Format line (design.md, Format and upgrade). Ported from
# Go's shclw/info.go, which has the reasons.

_INFO_TITLE = " This config file format is SHCL."
_INFO_NAME = ' "Simple Hierarchical Config Language"'
_FORMAT_WORD = "##    Format"
_MIGRATED_LINE = "##    Migrated from SHCL 2.x."
_SCHEMA_HEAD = "##    Schema   "
_INFO_MARK = "# shclw: the info block goes here"


class _Info(Enum):
	AsIs = 0
	On = 1
	Off = 2


class _Lines:
	"""Text as lines, each with its own end, so a CRLF file keeps them."""

	def __init__(self, text: str) -> None:
		self.lines: list[str] = []
		self.ends: list[str] = []
		crlf = 0
		at = 0
		while at < len(text):
			i = text.find("\n", at)
			if i < 0:
				self.lines.append(text[at:])
				self.ends.append("")
				break
			line, end = text[at:i], "\n"
			if line.endswith("\r"):
				line, end = line[:-1], "\r\n"
				crlf += 1
			self.lines.append(line)
			self.ends.append(end)
			at = i + 1
		self.eol = "\r\n" if crlf * 2 > len(self.lines) else "\n"

	def trimmed(self, i: int) -> str:
		return self.lines[i].strip(" \t\r")

	def join(self, drop: list[bool] | None = None) -> str:
		return "".join(
			ln + end
			for i, (ln, end) in enumerate(zip(self.lines, self.ends, strict=True))
			if drop is None or not drop[i]
		)

	def add(self, line: str) -> None:
		if self.ends and self.ends[-1] == "":
			self.ends[-1] = self.eol
		self.lines.append(line)
		self.ends.append(self.eol)

	def remove(self, drop: list[bool]) -> None:
		lines: list[str] = []
		ends: list[str] = []
		n = len(self.lines)
		i = 0
		while i < n:
			if not drop[i] and not _blank(self.lines[i]):
				lines.append(self.lines[i])
				ends.append(self.ends[i])
				i += 1
				continue
			k, gone, first_blank = i, False, -1
			while k < n and (drop[k] or _blank(self.lines[k])):
				gone = gone or drop[k]
				if not drop[k] and first_blank < 0:
					first_blank = k
				k += 1
			if not gone:
				lines += self.lines[i:k]
				ends += self.ends[i:k]
			elif first_blank >= 0 and lines and k < n:
				lines.append(self.lines[first_blank])
				ends.append(self.ends[first_blank])
			i = k
		self.lines, self.ends = lines, ends

	def trim_blank_tail(self) -> None:
		while self.lines and _blank(self.lines[-1]):
			self.lines.pop()
			self.ends.pop()


def _blank(line: str) -> bool:
	return line.strip(" \t\r") == ""


def _span(n: int, start: int, end: int) -> list[bool]:
	return [start <= i < end for i in range(n)]


def _format_number(trimmed: str, major: int) -> tuple[int, bool]:
	if trimmed == _FORMAT_WORD:
		return 0, True
	if not trimmed.startswith(_FORMAT_WORD + "   "):
		return 0, False
	digits = trimmed[len(_FORMAT_WORD) + 3 :]
	if not digits or digits.strip("0123456789"):
		return 0, True
	n = int(digits)
	return (major if n > 0xFFFFFFFF else n), True


def _info_runs(f: _Lines) -> list[tuple[int, int]]:
	runs: list[tuple[int, int]] = []
	n = len(f.lines)
	i = 0
	while i < n:
		t = f.trimmed(i)
		mark = "##" if t == "##" + _INFO_TITLE else "#" if t == "#" + _INFO_TITLE else ""
		if not mark:
			end = i + 1
			if _format_number(t, 0)[1]:
				if end < n and f.trimmed(end) == _MIGRATED_LINE:
					end += 1
				runs.append((i, end))
			i = end
			continue
		start, end = i, i + 1
		if i > 0 and f.trimmed(i - 1) == mark and (not runs or runs[-1][1] < i):
			start = i - 1
		closed = False
		k = end
		while k < n and f.trimmed(k).startswith(mark):
			if f.trimmed(k) == mark:
				end, closed = k + 1, True
				break
			k += 1
		while (
			not closed and end < n and (f.trimmed(end).startswith(mark + "    ") or f.trimmed(end) == mark + _INFO_NAME)
		):
			end += 1
		runs.append((start, end))
		i = end
	return runs


def _formats_named(f: _Lines, base: str) -> list[int]:
	named = []
	for i in range(len(f.lines)):
		n, ok = _format_number(f.trimmed(i), FORMAT_MAJOR)
		if not ok or n == 0 or _values(f.join(_span(len(f.lines), i, i + 1))) != base:
			continue
		named.append(n)
	return named


def _finish_info(text: str, info: _Info, stamp: bool) -> str:
	if info is _Info.AsIs and not stamp:
		return text
	bom = ""
	if text.startswith("﻿"):
		bom, text = "﻿", text[1:]
	f = _Lines(text)
	base = _values(text)
	named = False
	for n in _formats_named(f, base):
		if n > FORMAT_MAJOR:
			raise Error(ErrKind.TooNew, msg=f"the file names format {n}")
		if n < FORMAT_MAJOR:
			raise Error(ErrKind.Refused, msg=f"the file names format {n}; upgrade it first")
		named = True
	if info is not _Info.AsIs:
		named = False
		drop = [False] * len(f.lines)
		for start, end in _info_runs(f):
			n, lone = _format_number(f.trimmed(start), FORMAT_MAJOR)
			if lone and stamp and info is _Info.Off and n == FORMAT_MAJOR:
				named = True
				continue
			run = _span(len(f.lines), start, end)
			for i in range(start, end):
				run[i] = not f.trimmed(i).startswith(_SCHEMA_HEAD)
			if _values(f.join(run)) == base:
				drop = [a or b for a, b in zip(drop, run, strict=True)]
		f.remove(drop)
	before = f.join()
	if info is _Info.On:
		f.trim_blank_tail()
		if f.lines:
			f.add("")
		for line in shcl.GEN_BANNER.removesuffix("\n").split("\n"):
			f.add(line)
		named = True
	if stamp and not named:
		f.add(f"{_FORMAT_WORD}   {FORMAT_MAJOR}")
	out = f.join()
	if out != before and _values(out) != _values(before):
		raise Error(ErrKind.Refused, msg="the file ends in a raw block that never closes")
	return bom + out


def _old_block(text: str) -> tuple[str, bool]:
	body = text.removeprefix("﻿")
	if "#" + _INFO_TITLE not in body:
		return text, False
	f = _Lines(body)
	base = _values(body)
	for start, end in _info_runs(f):
		if f.trimmed(start) != "#" + _INFO_TITLE and (start + 1 == end or f.trimmed(start + 1) != "#" + _INFO_TITLE):
			continue
		run = _span(len(f.lines), start, end)
		last = -1
		for i in range(start, end):
			run[i] = not f.trimmed(i).startswith(_SCHEMA_HEAD)
			if run[i]:
				last = i
		if _values(f.join(run)) != base:
			continue
		if any(f.trimmed(i) == _INFO_MARK for i in range(len(f.lines))):
			return text, True
		f.lines[last] = _INFO_MARK
		run[last] = False
		return text[: len(text) - len(body)] + f.join(run), True
	return text, False


def _place_block(text: str, marked: bool) -> str:
	off = _finish_info(text, _Info.Off, False)
	body = off.removeprefix("﻿")
	head = off[: len(off) - len(body)]
	f = _Lines(body)
	at, marks = -1, [False] * len(f.lines)
	for i in range(len(f.lines)):
		if f.trimmed(i) == _INFO_MARK:
			at, marks[i] = i, True
	if not marked or at < 0 or body.count(_INFO_MARK) != 1:
		if marked:
			off = head + f.join(marks)
		return _finish_info(off, _Info.On, False)
	parts: list[str] = []
	block = shcl.GEN_BANNER.removesuffix("\n").split("\n")
	for i, (line, end) in enumerate(zip(f.lines, f.ends, strict=True)):
		if i != at:
			parts.append(line + end)
			continue
		for k, b in enumerate(block):
			parts.append(b + (end if k == len(block) - 1 else f.eol))
	out = "".join(parts)
	if _values(out) != _values(body):
		raise Error(ErrKind.Refused, msg="the file ends in a raw block that never closes")
	return head + out


def _keep_blanks(orig: str, up: str) -> str:
	"""up, shcl's conversion of orig, with the blank lines it dropped put back,
	so each line keeps its number (2026102618065634). Go's KeepBlanks has the
	reasons."""
	bom = "\ufeff" if up.startswith("\ufeff") else ""
	was, now = _Lines(orig.removeprefix("\ufeff")), _Lines(up.removeprefix(bom))
	gaps: list[list[str]] = []
	texts: list[str] = []
	gap: list[str] = []
	for line in was.lines:
		if line.strip(" \t"):
			gaps.append(gap)
			texts.append(line.strip(" \t"))
			gap = []
		else:
			gap.append(line)
	rest = [i for i, line in enumerate(now.lines) if line.strip(" \t")]
	parts: list[str] = []
	k = seen = n = 0
	for line, end in zip(now.lines, now.ends, strict=True):
		text = line.strip(" \t")
		if not text:
			seen += 1
		else:
			n += 1
			after = now.lines[rest[n]].strip(" \t") if n < len(rest) else ""
			if k < len(texts) and text != texts[k] and after == texts[k]:
				# A line the conversion added.
				seen = 0
			else:
				if k < len(gaps):
					parts += [b + now.eol for b in gaps[k][min(seen, len(gaps[k])) :]]
				k, seen = k + 1, 0
		parts.append(line + end)
	out = bom + "".join(parts)
	if out == up or _values(out) != _values(up):
		return up
	return out


def _same_ends(orig: str, text: str) -> str:
	if _Lines(orig).eol != "\r\n" or "\r" in text:
		return text
	crlf = text.replace("\n", "\r\n")
	return crlf if _values(crlf) == _values(text) else text


def _values(text: str) -> str:
	"""Every field's path and value and the load's error codes, to tell a
	comment from a raw body's line."""
	d = shcl.Document.parse(text)
	parts = [f"{x.path}\x00{x.value}\x01" for x in d.fields()]
	parts += [x.code + "\x02" for x in d.diagnostics() if x.severity == shcl.Severity.Error]
	return "".join(parts)


# The 3.0 adapter.

_STATUS = {s.name: s for s in Status}


def _got(r: Any, zero: Any) -> Got[Any]:
	return Got(
		r.value if r.value is not None else zero,
		_STATUS.get(r.status.name, Status.BadPath),
		r.line,
		r.raw if r.raw is not None else "",
	)


def _diags(ds: Sequence[Any]) -> list[Diag]:
	return [
		Diag(d.line, Severity.Error if d.severity == shcl.Severity.Error else Severity.Hint, d.code, d.message)
		for d in ds
	]


def _undiags(ds: Sequence[Diag]) -> list[Any]:
	sev = {Severity.Error: shcl.Severity.Error, Severity.Hint: shcl.Severity.Hint}
	return [shcl.Diagnostic(d.line, sev[d.severity], d.message, d.code) for d in ds]


def _strictness(s: Strictness) -> Any:
	return {Strictness.Loose: shcl.Strictness.Loose, Strictness.Strict: shcl.Strictness.Strict}.get(
		s, shcl.Strictness.Standard
	)


def _too_new(path: str, f: int) -> Error:
	return Error(ErrKind.TooNew, path, f"format {f}, and this shcl reads up to {FORMAT_MAJOR}")


@dataclass
class _UpgradedFile:
	"""The format open() upgraded the file from, and where the first write over
	it put the original."""

	from_: int
	backup: str = ""


def _ident(path: str) -> tuple[int, int] | None:
	try:
		st = os.stat(path)
	except OSError:
		return None
	return (st.st_dev, st.st_ino)


@dataclass
class _ReadFile:
	"""The file open() read, as it was then, so write() can tell it changed
	since. After a write over it, it's what that write left. gone is an
	open_layers() user file that wasn't there."""

	path: str
	text: str
	ident: tuple[int, int] | None
	gone: bool = False

	def replace(
		self, path: str, text: str, backup: Callable[[], str] | None, force: bool, write: Callable[[str, str], None]
	) -> str:
		"""Writes text over the file open() read, unless it changed since. Only
		the moment between the last check and the rename is left open
		(design.md, Loading and saving)."""
		was = self.text
		if force:
			try:
				was = Path(path).read_bytes().decode("utf-8")
			except (OSError, UnicodeDecodeError):
				write(path, text)
				return ""
		elif self.gone:
			if os.path.lexists(path):
				raise _changed(path)
			write(path, text)
			return ""
		return _replace_read(path, was, text, backup, write)

	def over(self, path: str) -> bool:
		if os.path.abspath(path) == os.path.abspath(self.path):
			return True
		try:
			st = os.stat(path)
		except OSError:
			return False
		return self.ident is not None and (st.st_dev, st.st_ino) == self.ident


@dataclass
class _LayerSet:
	docs: list[Doc | None]
	edit: int


def _set_err(path: str, st: Any) -> None:
	if st == shcl.SetStatus.Ok:
		return
	path_faults = {
		"BadPath",
		"ValueInPath",
		"Wildcard",
		"NoSuchIndex",
		"TooDeep",
		"Multiple",
		"UnderArray",
		"HasChildren",
		"RawBlock",
	}
	kind = ErrKind.BadPath if st.name in path_faults else ErrKind.BadValue
	raise Error(kind, path, st.name)


class Doc:
	"""A parsed document. Use new(), parse(), open() and the rest to get one."""

	def __init__(self, inner: Any) -> None:
		self._d = inner
		self._info = _Info.AsIs
		self._stamp = False
		self._up: _UpgradedFile | None = None
		self._file: _ReadFile | None = None
		self._newer = 0  # the newer format the text names, which makes it read-only
		self._lay: _LayerSet | None = None
		self._src = ""  # the text as loaded, for merged()
		self._level = Strictness.Standard
		self._loaded: list[Any] | None = None  # the load's diagnostics, once a fill-in parsed the text again
		self._backup = ""  # where a write copied the file open() read, with the lines the load lost

	@property
	def inner(self) -> Any:
		"""The shcl document underneath, for calls shclw doesn't cover. On a
		layered document it's the merge, built again after every edit."""
		return self._d

	def _top(self) -> Doc:
		if self._lay is None:
			return self
		top = self._lay.docs[self._lay.edit]
		assert top is not None
		return top

	def _remerge(self) -> None:
		assert self._lay is not None
		m = shcl.Document.new()
		for x in self._lay.docs:
			if x is not None:
				m.merge(x._d)
		self._d = m

	def _edited(self) -> None:
		if self._lay is not None:
			self._remerge()

	def _lines(self) -> str:
		text, ok = self._d.to_text_keep_lines()
		return text if ok else self._d.to_canonical()

	def _finish(self, text: str) -> str:
		try:
			return _saved(text, self._info, self._stamp)
		except Error:
			return text

	def text(self) -> str:
		"""The document as it would be saved: its lines as typed plus the edits,
		or the canonical form where 3.0 can't keep the lines."""
		t = self._top()
		return t._finish(t._lines())

	def canonical(self) -> str:
		t = self._top()
		return t._finish(t._d.to_canonical())

	def write(
		self, path: str, force: bool = False, *, create: bool = False, backup: bool = False, mode: int | None = None
	) -> None:
		"""Saves text() atomically. Refuses with Refused when the load dropped
		lines the save would lose. The first write over a file open() upgraded
		backs up the original first, and backup_name() names the copy. A write
		over the file open() read is Changed when it changed on disk since,
		unless force, and a document read from a newer format is TooNew.

		create writes only where nothing is there yet, and is Exists otherwise,
		even with 2 programs creating the file at once. mode is the permission
		bits a file this write creates gets, exactly, with no umask. A file
		that's already there keeps its own mode. Windows ignores it.

		backup writes over the file open() read where it would be Refused,
		after copying the file aside as the upgrade does. Later writes then
		don't refuse, since the lines are in the copy. Anywhere else there's no
		copy to make, and it's still Refused."""
		if self._lay is not None:
			self._top().write(path, force, create=create, backup=backup, mode=mode)
			return
		if self._newer:
			raise _too_new(path, self._newer)
		_check_mode(path, mode)
		f = self._file
		over = not create and f is not None and not f.gone and f.over(path)
		text, kept = self._d.to_text_keep_lines()
		lost = self._d.lost_count()
		keep_lost = not kept and lost > 0 and not self._backup
		if keep_lost and not (backup and over):
			raise Error(ErrKind.Refused, path, f"{lost} lines would be lost")
		if not kept:
			text = self._d.to_canonical()
		try:
			text = _saved(text, self._info, self._stamp)
		except Error as e:
			e.path = path
			raise
		if create:
			create_new(path, text, mode)
			if f is not None and f.over(path):
				f.text, f.gone, f.ident = text, False, _ident(path)
			return
		write = _write_mode(mode)
		if f is None or not f.over(path):
			write(path, text)
			return
		up = self._up
		if up is not None and not up.backup:
			up.backup = f.replace(path, text, _keep_original(path, f.text, up.from_), force, write)
			self._backup = up.backup
		elif keep_lost:
			self._backup = f.replace(path, text, _keep_original(path, f.text, FORMAT_MAJOR), force, write)
		else:
			f.replace(path, text, None, force, write)
		f.text, f.gone, f.ident = text, False, _ident(path)

	def read_only(self) -> bool:
		"""Whether the document was read from a newer format, which write()
		refuses with TooNew."""
		return self._top()._newer > 0

	def rewrites(self) -> bool:
		"""Whether write() puts the canonical form in place of the lines the
		document was loaded from."""
		d = self._top()
		if d._up is not None and not d._up.backup:
			return True
		return not d._d.to_text_keep_lines()[1]

	def upgraded_from(self) -> int:
		"""The format open() upgraded the file from, or 0."""
		up = self._top()._up
		return up.from_ if up is not None else 0

	def backup_name(self) -> str:
		"""Where a write copied the file open() read, or "". That's the first
		write over an upgraded file, or one with backup that would have lost
		lines."""
		return self._top()._backup

	def info_block(self, on: bool) -> None:
		"""Puts the info block at the end of what the save writes, or with on
		False leaves it out. An old block comes off first either way."""
		d = self._top()
		info = _Info.On if on else _Info.Off
		_finish_info(d._lines(), info, d._stamp)
		d._info = info

	def stamp_format(self) -> None:
		"""Adds the Format line at the end of what the save writes."""
		d = self._top()
		_finish_info(d._lines(), d._info, True)
		d._stamp = True

	def diags(self) -> list[Diag]:
		if self._lay is None:
			return _diags(self._loaded if self._loaded is not None else self._d.diagnostics())
		out = []
		for i, x in enumerate(self._lay.docs):
			if x is not None:
				for g in x.diags():
					g.layer = i + 1
					out.append(g)
		return out

	def unused(self) -> list[Spot]:
		"""The lines the load couldn't use, which are the lines with an error,
		once each."""
		return _unused_of(self.diags())

	def merged(self) -> list[Merge]:
		"""Each name the load kept one copy of, since a later copy had the same
		name and value, with every copy's line. On a layered document it's
		each layer's own, and not what the layers' merge folded."""
		if self._lay is None:
			return _merges_of(self._src, 0)
		out = []
		for i, x in enumerate(self._lay.docs):
			if x is not None:
				out += _merges_of(x._src, i + 1)
		return out

	def errors(self) -> int:
		if self._lay is None:
			if self._loaded is not None:
				return sum(1 for g in self._loaded if g.severity == shcl.Severity.Error)
			return int(self._d.error_count())
		return sum(x.errors() for x in self._lay.docs if x is not None)

	def commented(self) -> list[Commented]:
		"""Each comment line that reads as one key: value once its # comes off,
		with the path it would set. A layered document's are the edit layer's."""
		return _commented_of(self._top()._lines())

	def _fill_in(self, path: str) -> tuple[Any, list[Any] | None] | None:
		"""Swaps path's commented ## Default line for the setting, so the set
		that follows takes that line and the ones under it don't move
		(2026101513330020). Only where the save keeps the file's lines and path
		isn't set yet. It gives back what _unfill takes to put the document
		back, for a set that fails."""
		if self._d.exists(path):
			return None
		text, kept = self._d.to_text_keep_lines()
		if not kept:
			return None
		line = _default_line(text, path)
		if not line:
			return None
		try:
			nd = shcl.Document.parse_keep_lines(_uncommented(text, line), _strictness(self._level))
		except shcl.LoadError as e:
			nd = e.document
		if nd is None or nd.error_count() != self._d.error_count() or not nd.exists(path):
			return None
		was = (self._d, self._loaded)
		if self._loaded is None:
			self._loaded = list(self._d.diagnostics())
		self._d = nd
		return was

	def _unfill(self, was: tuple[Any, list[Any] | None] | None) -> None:
		if was is not None:
			self._d, self._loaded = was

	def check(self, schema: Doc) -> list[Diag]:
		"""Validates the document against a schema document."""
		return _diags(self._d.validate(schema._d))

	def drop_declared(self, ds: list[Diag]) -> list[Diag]:
		"""ds without the repeat and reopen hints for keys this schema says repeat."""
		inner = _undiags(ds)
		shcl.suppress_declared_repeats(self._d, inner)
		shcl.suppress_declared_reopens(self._d, inner)
		return _diags(inner)

	def int_or(self, path: str, fallback: int) -> int:
		g = self.get_int(path)
		return g.value if g.ok() else fallback

	def float_or(self, path: str, fallback: float) -> float:
		g = self.get_float(path)
		return g.value if g.ok() else fallback

	def bool_or(self, path: str, fallback: bool) -> bool:
		g = self.get_bool(path)
		return g.value if g.ok() else fallback

	def str_or(self, path: str, fallback: str) -> str:
		g = self.get_str(path)
		return g.value if g.ok() else fallback

	def strs_or(self, path: str, fallback: list[str]) -> list[str]:
		g = self.get_strs(path)
		return g.value if g.ok() else fallback

	def ints_or(self, path: str, fallback: list[int]) -> list[int]:
		g = self.get_ints(path)
		return g.value if g.ok() else fallback

	def floats_or(self, path: str, fallback: list[float]) -> list[float]:
		g = self.get_floats(path)
		return g.value if g.ok() else fallback

	def raw_or(self, path: str, fallback: str) -> str:
		g = self.get_raw(path)
		return g.value if g.ok() else fallback

	def get_int(self, path: str) -> Got[int]:
		return _got(self._d.read_int(path), 0)

	def get_float(self, path: str) -> Got[float]:
		g = _got(self._d.read_float(path), 0.0)
		g.value = float(g.value)
		return _finite_float(g)

	def get_bool(self, path: str) -> Got[bool]:
		return _got(self._d.read_bool(path), False)

	def get_str(self, path: str) -> Got[str]:
		return _got(self._d.read_string(path), "")

	def get_strs(self, path: str) -> Got[list[str]]:
		return _got(self._d.read_string_array(path), [])

	def get_ints(self, path: str) -> Got[list[int]]:
		return _got(self._d.read_int_array(path), [])

	def get_floats(self, path: str) -> Got[list[float]]:
		g = _got(self._d.read_float_array(path), [])
		g.value = [float(x) for x in g.value]
		return _finite_floats(g)

	def get_raw(self, path: str) -> Got[str]:
		return _got(self._d.read_raw(path), "")

	def has(self, path: str) -> bool:
		return bool(self._d.exists(path))

	def count(self, path: str) -> int:
		return int(self._d.count(path))

	def line(self, path: str) -> int:
		return int(self._d.line(path))

	def names(self, path: str) -> list[str]:
		"""The child names under path once each, in file order."""
		return _dedupe_names(self._d.children(path))

	def each(self, path: str) -> list[Node]:
		"""Every instance path names, each with a path that reads and edits it alone."""
		fields = self._d.read_fields(path).value or []
		return [Node(f.path, f.value, f.line) for f in fields if f.path]

	def child(self, base: str, name: str) -> str:
		"""base with one more name on the end, quoted if it needs it."""
		q = shcl.quote_segment(name)
		return q if not base else f"{base}.{q}"

	def _in_file_order(self, path: str) -> list[Node]:
		# An instance a put made has no line, and goes last.
		return sorted(self.each(path), key=lambda n: n.line or sys.maxsize)

	def first(self, path: str) -> str:
		"""path narrowed to its first instance in the file, or path as it is."""
		nodes = self._in_file_order(path)
		return nodes[0].path if nodes else path

	def last(self, path: str) -> str:
		nodes = self._in_file_order(path)
		return nodes[-1].path if nodes else path

	def lines(self, path: str) -> list[int]:
		"""The line of each instance of path, in file order."""
		return [n.line for n in self._in_file_order(path)]

	def put(self, path: str, value: object) -> None:
		"""Sets path to an int, float, bool, str, or a list of one of those but
		bool. A value that doesn't read back the same is NoReadBack, with the
		edit left in place. An empty list writes an empty array."""
		if self._lay is not None:
			try:
				self._top().put(path, value)
			finally:
				self._edited()
			return
		kind, v = _normalize(path, value)
		was = self._fill_in(path)
		setter = {
			_Kind.Int: self._d.set_int,
			_Kind.Float: self._d.set_float,
			_Kind.Bool: self._d.set_bool,
			_Kind.Str: self._d.set_string,
			_Kind.Strs: self._d.set_string_array,
			_Kind.Ints: self._d.set_int_array,
			_Kind.Floats: self._d.set_float_array,
		}[kind]
		try:
			_set_err(path, setter(path, v))
		except Error:
			self._unfill(was)
			raise
		self._verify_put(path, kind, v)

	def _verify_put(self, path: str, kind: _Kind, v: Any) -> None:
		read = {
			_Kind.Int: self.get_int,
			_Kind.Float: self.get_float,
			_Kind.Bool: self.get_bool,
			_Kind.Str: self.get_str,
			_Kind.Strs: self.get_strs,
			_Kind.Ints: self.get_ints,
			_Kind.Floats: self.get_floats,
		}[kind]
		g = read(path)
		if isinstance(v, list) and not v and g.status is Status.Empty:
			return
		if g.status is Status.Good and g.value == v:
			return
		msg = "reads back as a different value" if g.status is Status.Good else f"reads back as {g.status.name}"
		raise Error(ErrKind.NoReadBack, path, msg)

	def put_syntax(self, path: str, text: str) -> None:
		"""Sets path to text read as shcl value syntax. Text that doesn't read
		back as typed, such as one with a trailing comment, is NoReadBack."""
		if self._lay is not None:
			try:
				self._top().put_syntax(path, text)
			finally:
				self._edited()
			return
		was = self._fill_in(path)
		try:
			_set_err(path, self._d.set_literal(path, text))
		except Error:
			self._unfill(was)
			raise
		alone = new()
		# A failed set leaves alone empty, which the check reports.
		alone._d.set_literal("k", text)
		line = parse(f"k: {text}\n")
		if alone.canonical() != line.canonical():
			raise Error(ErrKind.NoReadBack, path, "the text doesn't read back as typed")
		got, want = self.get_str(path), line.get_str("k")
		if got.status is not want.status or got.value != want.value:
			raise Error(ErrKind.NoReadBack, path, "reads back as a different value")

	def comment(self, path: str, text: str) -> None:
		"""Sets the comment above path."""
		try:
			t = self._top()
			_set_err(path, t._d.set_comment(path, text))
		finally:
			self._edited()

	def remove(self, path: str) -> int:
		"""Takes out path and everything under it, and says how many instances went."""
		try:
			return int(self._top()._d.remove(path))
		finally:
			self._edited()


def _saved(text: str, info: _Info, stamp: bool) -> str:
	out = _finish_info(text, info, stamp)
	if format_of(out) != 0:
		return out
	marked, found = _old_block(out)
	if not found:
		return out
	return _place_block(marked, marked != out)


def _write_atomic(path: str, text: str) -> None:
	try:
		shcl.write_file_atomic(path, text)
	except shcl.WriteError as e:
		kind = ErrKind.NotFound if e.status == shcl.WriteStatus.NotFound else ErrKind.IO
		raise Error(kind, path) from e


def _keep_original(path: str, text: str, from_: int) -> Callable[[], str]:
	def keep() -> str:
		try:
			return str(shcl.write_backup(path, text, from_))
		except shcl.UpgradeBackupTaken as e:
			raise Error(ErrKind.BackupTaken, path) from e
		except (shcl.UpgradeError, OSError) as e:
			raise Error(ErrKind.IO, path) from e

	return keep


def new() -> Doc:
	"""An empty document. It keeps lines like a loaded one, so rewrites() is False."""
	return parse("")


def parse(text: str) -> Doc:
	"""Never fails. Problems are in diags()."""
	return _parse_keep(text, Strictness.Standard)[0]


def _parse_keep(text: str, s: Strictness) -> tuple[Doc, Error | None]:
	err = None
	try:
		d = Doc(shcl.Document.parse_keep_lines(text, _strictness(s)))
	except shcl.LoadError as e:
		inner = e.document if e.document is not None else shcl.Document.new()
		err = Error(ErrKind.Strict, diags=_diags(e.diagnostics))
		err.__cause__ = e
		d = Doc(inner)
	d._src = text
	d._level = s
	f = format_of(text)
	d._newer = f if f > FORMAT_MAJOR else 0
	return d, err


def _unused_of(ds: list[Diag]) -> list[Spot]:
	"""The lines with an error in ds, once each, by layer then line. An error on
	no line, such as one about the whole file, has no line to give."""
	return sorted({Spot(g.layer, g.line) for g in ds if g.severity is Severity.Error and g.line > 0})


def _walk(x: Doc, path: str, see: Callable[[str, str, Node], bool]) -> None:
	for name in x.names(path):
		for n in x.each(x.child(path, name)):
			if see(path, name, n):
				_walk(x, n.path, see)


def _commented_of(text: str) -> list[Commented]:
	"""The commented settings in text. Each candidate line is uncommented alone
	and the text parsed again, so the path is shcl's own reading of where the
	line sits. A line is left out when that adds an error or changes any other
	field, such as one that would open a raw block or take the lines under it
	as children."""
	return _commented_where(text, lambda _line, _body: True)


def _default_line(text: str, path: str) -> int:
	"""The line of path's commented ## Default setting in text, or 0. Only the
	lines whose key path ends with are parsed."""
	want = _ascii_lower(path)

	def keep(line: str, body: str) -> bool:
		return _default_note(line) is not None and _key_ends(path, body)

	return next((c.line for c in _commented_where(text, keep) if _ascii_lower(c.path) == want), 0)


def _key_ends(path: str, body: str) -> bool:
	"""A quick test, before the parse that says for sure, of whether path ends
	with the key body starts with."""
	end = body.find(":")
	if body[0] in "\"'":
		c = body.find(body[0], 1)
		if c >= 0:
			k = body.find(":", c + 1)
			if k >= 0:
				end = k
	key = _ascii_lower(body[:end].rstrip(" \t"))
	p = _ascii_lower(path)
	return p == key or p.endswith("." + key)


def _commented_where(text: str, keep: Callable[[str, str], bool]) -> list[Commented]:
	lines = text.split("\n")
	cands = []
	for i, line in enumerate(lines):
		u = _uncomment(line, False)
		if u is not None and keep(line, u.lstrip(" \t")):
			cands.append(i)
	out: list[Commented] = []
	if not cands:
		return out
	d = parse(text)
	base = _fields_of(d, 0)
	errs = d.errors()
	for i in cands:
		probe = list(lines)
		probe[i] = _uncomment(lines[i], False) or lines[i]
		p = parse("\n".join(probe))
		n = i + 1
		if p.errors() != errs or _fields_of(p, n) != base:
			continue
		c = _leaf_at(p, n)
		if c is not None:
			out.append(Commented(c.path, c.value, n, _default_note(lines[i]) is not None))
	return out


def _uncommented(text: str, n: int) -> str:
	"""text with line n's # taken off, and its ## Default note too, as a
	fill-in writes it."""
	lines = text.split("\n")
	lines[n - 1] = _uncomment(lines[n - 1], True) or lines[n - 1]
	return "\n".join(lines)


def _uncomment(line: str, drop_note: bool) -> str | None:
	"""line without its #, for a line with one # and then text with a colon in
	it. A ## line is a note, not a setting."""
	body = line.lstrip(" \t")
	indent = line[: len(line) - len(body)]
	if not body.startswith("#") or body.startswith("##"):
		return None
	body = body[1:].lstrip(" \t")
	if not body or body.startswith("#") or ":" not in body:
		return None
	if drop_note:
		at = _default_note(body)
		if at is not None:
			eol = "\r" if body.endswith("\r") else ""
			body = body[:at].rstrip(" \t") + eol
	return indent + body


def _default_note(line: str) -> int | None:
	"""Where a trailing ## Default note starts in line."""
	t = line.rstrip(" \t\r")
	if len(t) < 7 or _ascii_lower(t[-7:]) != "default":
		return None
	t = t[:-7].rstrip(" \t")
	if not t.endswith("##"):
		return None
	at = len(t) - 2
	return at if at > 0 and t[at - 1] in " \t" else None


def _fields_of(d: Doc, skip: int) -> list[tuple[str, int]]:
	"""Every field's name and line in shcl's order, less the ones on line skip."""
	out: list[tuple[str, int]] = []

	def see(_: str, name: str, n: Node) -> bool:
		if n.line != skip:
			out.append((_ascii_lower(name), n.line))
		return True

	_walk(d, "", see)
	return out


def _leaf_at(d: Doc, n: int) -> Commented | None:
	"""The one field on line n with nothing under it, with its path as a
	program would write it: no index on the last step, which a key already set
	elsewhere in the block gets once the line comes back."""
	found: list[Commented] = []

	def see(parent: str, name: str, x: Node) -> bool:
		if x.line == n and not d.names(x.path):
			found.append(Commented(d.child(parent, name), x.value, n))
		return True

	_walk(d, "", see)
	return found[0] if len(found) == 1 else None


def _probe_name(line: int) -> str:
	return f"shclwprobe{line}x"


def _merges_of(text: str, layer: int) -> list[Merge]:
	"""The copies shcl folded into an earlier one in text, apart or not (shcl's
	H002 only says so when they're apart). A folded copy's line is on no node,
	so each such line gets its name made unique and the text is parsed again,
	where the copy then stands alone (design.md, Decisions)."""
	d = parse(text)
	at: dict[int, str] = {}

	def see_d(_parent: str, _name: str, n: Node) -> bool:
		if n.line > 0:
			at[n.line] = n.path
		return True

	_walk(d, "", see_d)
	bad = {g.line for g in d.diags() if g.severity is Severity.Error}
	probe = text.split("\n")
	folded = set()
	for i, ln in enumerate(probe):
		n = i + 1
		body = ln.lstrip(" \t")
		if n in at or n in bad or not body:
			continue
		indent, c = ln[: len(ln) - len(body)], body[0]
		if c in "\"'":
			probe[i] = indent + c + _probe_name(n) + body[1:]
		elif ord(c) >= 0x80 or ("a" <= c.lower() <= "z"):
			probe[i] = indent + _probe_name(n) + body
		else:
			continue
		folded.add(n)
	out: list[Merge] = []
	if not folded:
		return out
	p = parse("\n".join(probe))
	slot: dict[str, int] = {}
	# The path in d of the node a probe node stands for.
	orig = {"": ""}

	def see_p(parent: str, name: str, n: Node) -> bool:
		if n.line not in folded:
			orig[n.path] = at.get(n.line, "")
			return n.line in at
		if parent not in orig:
			return False
		pre = _probe_name(n.line)
		if name.lower().startswith(pre):
			name = name[len(pre) :]
		kept = None
		for k in d.each(d.child(orig[parent], name)):
			if k.value == n.value and 0 < k.line < n.line and (kept is None or k.line > kept.line):
				kept = k
		if kept is None:
			return False
		orig[n.path] = kept.path
		# A dotted line folds each step; only its last one is the copy.
		for c in p.names(n.path):
			if any(k.line == n.line for k in p.each(p.child(n.path, c))):
				return True
		if kept.path not in slot:
			slot[kept.path] = len(out)
			out.append(Merge(kept.path, [kept.line], layer))
		out[slot[kept.path]].lines.append(n.line)
		return True

	_walk(p, "", see_p)
	for m in out:
		m.lines.sort()
	out.sort(key=lambda m: (m.lines[0], m.path.encode("utf-8", "surrogateescape")))
	return out


def parse_with(text: str, s: Strictness) -> Doc:
	"""Raises Error with kind Strict only at Strict. Its .doc is the document anyway."""
	d, err = _parse_keep(text, s)
	if err is not None:
		err.doc = d  # type: ignore[attr-defined]
		raise err
	return d


def open(path: str, s: Strictness = Strictness.Standard) -> Doc:
	"""Reads and parses a file. A missing file is NotFound. One written for a
	newer format is read as this shcl reads it, and is read-only: write() on
	it is TooNew. An older one is upgraded in memory, as upgrade()
	with from_ 0 does, and upgraded_from() says so. Nothing is written until
	write(), and the first write over the file backs it up. A 2.x file that
	reads two ways is Ambiguous, and one shcl 2.0 reports errors on, or that
	can't be made to read as 2.0 read it, is Refused, with the lines in diags.
	s applies to the upgraded text."""
	d, err = _open_keep(path, s)
	if err is not None:
		raise err
	return d


def _open_keep(path: str, s: Strictness) -> tuple[Doc, Error | None]:
	try:
		text = read_text(path)
		up = None
		body = text
		# A file naming this format or a newer one skips the upgrade's parse.
		if format_of(text) < FORMAT_MAJOR:
			u = upgrade(text, 0)
			if u.changed:
				up = _UpgradedFile(u.from_)
				body = u.text
	except Error as e:
		e.path = path
		return new(), e
	d, err = _parse_keep(body, s)
	d._up = up
	d._file = _ReadFile(path, text, _ident(path))
	if err is not None:
		err.path = path
	return d, err


def open_layers(*layers: Layer) -> Doc:
	"""Reads layers in order, lowest first, as their merge: a later layer's
	value, array, or raw block takes the place of a lower one's, and blocks
	merge name by name. A missing file is skipped. Edits and saves go to the
	last file layer; text layers after it still win reads. Each file is opened
	as open() does, and the first error is raised. Diags are every layer's,
	with layer set (design.md, Decisions)."""
	if not layers:
		return new()
	edit = len(layers) - 1
	for i in range(len(layers) - 1, -1, -1):
		if layers[i].file:
			edit = i
			break
	docs: list[Doc | None] = [None] * len(layers)
	for i, ly in enumerate(layers):
		if not ly.file:
			docs[i] = parse(ly.text)
			continue
		x, err = _open_keep(ly.file, Strictness.Standard)
		if err is None:
			docs[i] = x
		elif err.kind is ErrKind.NotFound:
			if i == edit:
				docs[i] = new()
				docs[i]._file = _ReadFile(ly.file, "", None, gone=True)
		else:
			for g in err.diags:
				g.layer = i + 1
			raise err
	d = Doc(None)
	d._lay = _LayerSet(docs, edit)
	d._remerge()
	return d


def format_of(text: str) -> int:
	"""The format a file's Format line names, or 0 with none."""
	n = shcl.format_version(text)
	return int(n) if n is not None else 0


def upgrade(text: str, from_: int = 0) -> Upgraded:
	"""Converts a file written for an older format. With from_ 0 the file's
	Format line decides, and a file with none is taken as 2.x when it has 2.x's
	info block, or errors on 3.0 that shcl 2.0 doesn't have. Pass 2 for a file
	known to be from shcl 2.x or older. Same rules as Go's shclw3.Upgrade."""
	f = max(format_of(text), from_)
	if f > FORMAT_MAJOR:
		raise _too_new("", f)
	src, banner = text, False
	if format_of(text) == 0:
		src, banner = _old_block(text)
		if banner and from_ == 0:
			from_ = 2
	from_v2 = 0 < from_ < FORMAT_MAJOR
	u = shcl.upgrade(src, from_v2)
	out = Upgraded(u.text, u.format, not u.current, u.lost, u.backup)
	# shcl says 2 for a file with no Format line, even one it leaves alone.
	if u.current and not from_v2 and format_of(text) == 0:
		out.from_ = FORMAT_MAJOR
	if u.current and not (from_v2 and u.format < FORMAT_MAJOR):
		out.text = text
		return out
	# Nothing here says 2.x but its errors on 3.0. With errors on 2.0 too it's
	# a current file with a typo (design.md, Decisions).
	old = shcl2.Document.parse(text)
	guessed = not from_v2 and format_of(text) == 0
	old_err = _old_errors(old)
	if guessed and old_err is not None:
		return Upgraded(text, FORMAT_MAJOR)
	if u.ambiguous > 0:
		raise Error(ErrKind.Ambiguous, msg=f"{u.ambiguous} lines read two ways")
	if old_err is not None:
		raise old_err
	# shcl gives the input back then, and this would read it the 2.x way.
	if u.errors_left:
		raise Error(
			ErrKind.Refused,
			msg=f"{len(u.errors_left)} lines would still have an error once converted; fix them and upgrade again",
		)
	if out.changed:
		out.text = _keep_blanks(src, out.text)
	out.lost = _check_reads(old, out.text, format_of(text) == 1)
	if banner:
		fixed = _place_block(_same_ends(text, out.text), src != text)
	elif not out.changed:
		return out
	else:
		fixed = _same_ends(text, out.text)
	out.text, out.changed = fixed, fixed != text
	out.rewritten = rewritten_lines(text, fixed, out.lost)
	return out


def upgrade_file(path: str, from_: int = 0) -> Upgraded:
	"""upgrade() on a file. When it changes the file it first copies the
	original beside it, and backup names the copy."""
	if from_ > FORMAT_MAJOR:
		raise _too_new(path, from_)
	# A FIFO would block the read, and the write would put a file in place of a device.
	try:
		mode = os.stat(path).st_mode
	except OSError:
		mode = stat.S_IFREG
	if not stat.S_ISREG(mode):
		raise Error(ErrKind.IO, path, "not a regular file")
	text = read_text(path)
	try:
		u = upgrade(text, from_)
	except Error as e:
		e.path = path
		raise
	if not u.changed:
		return u
	u.backup = _replace_read(path, text, u.text, _keep_original(path, text, u.from_), _write_atomic)
	return u


def _old_errors(old: Any) -> Error | None:
	errs = [
		Diag(d.line, Severity.Error, d.code, d.message) for d in old.diagnostics() if d.severity == shcl2.Severity.Error
	]
	if not errs:
		return None
	n = len({d.line for d in errs})
	return Error(ErrKind.Refused, msg=f"shcl 2.0 reports errors on {n} lines; fix them and upgrade again", diags=errs)


def _carry_diag(line: int, msg: str) -> Diag:
	return Diag(line, Severity.Error, "W002", msg)


def _check_reads(old: Any, up: str, names_as_typed: bool) -> list[int]:
	"""The lines with a value 2.0 read in old that up, the upgraded text,
	doesn't have at all. Refused when up reads a value another way
	(2026101910455900). Go's checkReads has the reasons."""
	now = shcl.Document.parse(up)
	bad: list[Diag] = []
	for f in now.fields():
		if not names_as_typed and _v2_escapes(f.name) != f.name:
			bad.append(
				_carry_diag(
					old.line(f.path),
					"shcl 2.0 read this name with its backslash escapes resolved, and 3.0 reads it as typed; "
					"rename it and upgrade again",
				)
			)
	differs = "the upgraded file doesn't read this value as shcl 2.0 read it"
	lost: set[int] = set()
	for p in old.paths():
		had, has = old.count(p), now.count(p)
		# Instances past the ones 3.0 still has are gone.
		lost.update(line for line in old.lines(p)[min(had, has) :] if line > 0)
		if had != 1:
			continue
		was, now_is = old.read_string_array(p), now.read_string_array(p)
		if was.status == shcl2.Status.Good and now_is.status == shcl.Status.Good and was.value != now_is.value:
			bad.append(_carry_diag(old.line(p), differs))
		if (
			has == 1
			and was.status == shcl2.Status.Good
			and now_is.status != shcl.Status.Good
			and now.read_raw(p).status != shcl.Status.Good
		):
			lost.add(old.line(p))
	if not bad:
		return sorted(lost)
	n = len({b.line for b in bad})
	msg = f"{n} lines don't read as shcl 2.0 read them; fix them and upgrade again"
	raise Error(ErrKind.Refused, msg=msg, diags=bad)


_V2_ESCAPES = {"t": "\t", "n": "\n", "\\": "\\", '"': '"', "'": "'"}


def _v2_escapes(s: str) -> str:
	"""s as shcl 1.2 and 2.0 read it: \\t, \\n, \\\\, \\", and \\' are escapes,
	and any other backslash is itself."""
	if "\\" not in s:
		return s
	out: list[str] = []
	i = 0
	while i < len(s):
		c = s[i]
		if c != "\\" or i + 1 == len(s):
			out.append(c)
			i += 1
			continue
		nxt = s[i + 1]
		out.append(_V2_ESCAPES.get(nxt, "\\" + nxt))
		i += 2
	return "".join(out)


# Ops scripts (design.md, Ops scripts): the driver language every wrapper
# shares, so the shared cases and the fuzzer can compare them.


def run(input_text: str, script: str, dir: str = "") -> str:
	"""Parses input_text, applies a script of ops to it, and returns what the
	ops printed. dir is where check reads and write writes; empty turns them off."""
	r = _Runner(input_text, dir)
	for raw_line in script.split("\n"):
		line = raw_line.rstrip("\r")
		if not line.strip(_GO_SPACE) or line.startswith("#"):
			continue
		r.out("> " + line)
		r.op([_unesc(x) for x in line.split("\t")])
	return "".join(r.b)


# What Go's strings.TrimSpace takes off, which Python's strip() doesn't match.
_GO_SPACE = "\t\n\v\f\r \x85\xa0\u1680" + "".join(map(chr, range(0x2000, 0x200B))) + "\u2028\u2029\u202f\u205f\u3000"
_INT_RE = re.compile(r"[+-]?[0-9]+")
_FLOAT_RE = re.compile(r"[+-]?(?:[0-9]+\.?[0-9]*|\.[0-9]+)(?:[eE][+-]?[0-9]+)?")


def _parse_int(s: str) -> int | None:
	if not _INT_RE.fullmatch(s):
		return None
	n = int(s)
	return n if -(1 << 63) <= n < (1 << 63) else None


def _parse_float(s: str) -> float | None:
	if s == "nan":
		return math.nan
	if s == "inf":
		return math.inf
	if s == "-inf":
		return -math.inf
	return float(s) if _FLOAT_RE.fullmatch(s) else None


def _parse_value(typ: str, f: list[str]) -> tuple[Any, bool]:
	first = f[0] if f else ""
	if typ == "int":
		n = _parse_int(first)
		return n, n is not None
	if typ == "float":
		x = _parse_float(first)
		return x, x is not None
	if typ == "bool":
		return first == "true", first in ("true", "false")
	if typ == "str":
		return first, True
	if typ == "strs":
		return list(f), True
	if typ in ("ints", "floats"):
		conv = _parse_int if typ == "ints" else _parse_float
		vals = [conv(s) for s in f]
		if any(v is None for v in vals):
			return None, False
		return vals, True
	return None, False


def _unesc(s: str) -> str:
	if "\\" not in s:
		return s
	out: list[str] = []
	i = 0
	while i < len(s):
		c = s[i]
		if c != "\\" or i + 1 == len(s):
			out.append(c)
			i += 1
			continue
		nxt = s[i + 1]
		out.append({"t": "\t", "n": "\n", "r": "\r", "\\": "\\"}.get(nxt, "\\" + nxt))
		i += 2
	return "".join(out)


_ESC = {"\\": "\\\\", "\t": "\\t", "\n": "\\n", "\r": "\\r"}
_NEEDS_ESC = re.compile(r"[\x00-\x1f\x7f\\\udc80-\udcff]")


def _esc(s: str) -> str:
	"""A value made safe for one tab-separated output field."""
	if not _NEEDS_ESC.search(s):
		return s

	def one(m: re.Match[str]) -> str:
		c = m.group()
		if c in _ESC:
			return _ESC[c]
		n = ord(c)
		# A byte that wasn't UTF-8, under surrogateescape.
		return f"\\x{n - 0xDC00 if n >= 0xDC80 else n:02x}"

	return _NEEDS_ESC.sub(one, s)


def _spot_text(line: int, layer: int) -> str:
	"""A line, with its layer in front on a layered document."""
	return f"{layer}:{line}" if layer > 0 else str(line)


def _counted(n: int, items: str) -> str:
	return "0" if n == 0 else f"{n}\t{items}"


def _join_floats(v: list[float]) -> str:
	return "\t".join(format_float(x) for x in v)


def _mask_stamp(name: str) -> str:
	at = name.find("_backup_")
	if at < 0:
		return name
	start = at + len("_backup_")
	end = name.find("_format-v", start)
	if end < 0:
		return name
	return name[:start] + "STAMP" + name[end:]


def _strictness_of(level: str) -> Strictness:
	return {"loose": Strictness.Loose, "strict": Strictness.Strict}.get(level, Strictness.Standard)


@dataclass
class _Runner:
	input: str
	dir: str
	doc: Doc = field(init=False)
	b: list[str] = field(default_factory=list)

	def __post_init__(self) -> None:
		self.doc = parse(self.input)

	def out(self, s: str) -> None:
		self.b.append(s + "\n")

	def result(self, err: Error | None) -> None:
		self.out("ok" if err is None else "error " + err.kind.value)

	def attempt(self, call: Callable[[], object]) -> None:
		try:
			call()
		except Error as e:
			self.result(e)
			return
		self.result(None)

	def op(self, f: list[str]) -> None:
		def arg(i: int) -> str:
			return f[i] if i < len(f) else ""

		d = self.doc
		name = f[0]
		if name == "parse":
			try:
				self.doc = parse_with(self.input, _strictness_of(arg(1)))
				self.result(None)
			except Error as e:
				self.doc = e.doc  # type: ignore[attr-defined]
				self.result(e)
		elif name == "text":
			self.block(d.text())
		elif name == "canonical":
			self.block(d.canonical())
		elif name == "rewrites":
			self.out(_bool(d.rewrites()))
		elif name == "diags":
			self.diags(d.diags())
		elif name == "errors":
			self.out(str(d.errors()))
		elif name == "unused":
			spots = d.unused()
			self.out(_counted(len(spots), "\t".join(_spot_text(x.line, x.layer) for x in spots)))
		elif name == "merged":
			for m in d.merged():
				self.out("\t".join([_esc(m.path)] + [_spot_text(n, m.layer) for n in m.lines]))
		elif name == "commented":
			for c in d.commented():
				self.out(f"{c.line}\t{_esc(c.path)}\t{_esc(c.value)}" + ("\tdefault" if c.default else ""))
		elif name == "get":
			self.out(self.get(arg(1), arg(2)))
		elif name == "or":
			self.or_(arg(1), arg(2), f[3:])
		elif name == "has":
			self.out(_bool(d.has(arg(1))))
		elif name == "count":
			self.out(str(d.count(arg(1))))
		elif name == "line":
			self.out(str(d.line(arg(1))))
		elif name == "lines":
			lines = d.lines(arg(1))
			self.out(_counted(len(lines), "\t".join(map(str, lines))))
		elif name == "first":
			self.out(self.get(arg(1), d.first(arg(2))))
		elif name == "last":
			self.out(self.get(arg(1), d.last(arg(2))))
		elif name == "names":
			self.out("\t".join(_esc(n) for n in d.names(arg(1))))
		elif name == "each":
			for i, n in enumerate(d.each(arg(1))):
				self.out(f"{i}\t{n.line}\t{_esc(n.value)}")
		elif name == "eachget":
			for i, n in enumerate(d.each(arg(2))):
				self.out(f"{i}\t" + self.get(arg(1), d.child(n.path, arg(3))))
		elif name == "put":
			v, ok = _parse_value(arg(1), f[3:])
			if not ok:
				self.out("error badvalue")
				return
			self.attempt(lambda: d.put(arg(2), v))
		elif name == "syntax":
			self.attempt(lambda: d.put_syntax(arg(1), arg(2)))
		elif name == "comment":
			self.attempt(lambda: d.comment(arg(1), arg(2)))
		elif name == "remove":
			self.out(str(d.remove(arg(1))))
		elif name == "info":
			if arg(1) not in ("on", "off"):
				self.out("error badvalue")
				return
			self.attempt(lambda: d.info_block(arg(1) == "on"))
		elif name == "stamp":
			self.attempt(d.stamp_format)
		elif name == "check":
			self.check(arg(1), arg(2))
		elif name == "format":
			self.out(str(format_of(self.input)))
		elif name == "upgrade":
			self.upgrade(arg(1))
		elif name == "write":
			self.write(arg(1), f[2:])
		elif name == "mode":
			self.mode(arg(1))
		elif name == "readonly":
			self.out(_bool(self.doc.read_only()))
		elif name == "scribble":
			self.scribble(arg(1), arg(2))
		elif name == "open":
			self.open(arg(1), arg(2))
		elif name == "layers":
			self.layers(f[1:])
		elif name == "upgraded":
			self.upgraded()
		elif name == "files":
			self.files()
		else:
			self.out("error unknownop")

	def file(self, name: str) -> str | None:
		"""A script's file name, kept inside dir."""
		if not self.dir or not name or name != os.path.basename(name) or name == "..":
			self.out("error unsupported")
			return None
		return os.path.join(self.dir, name)

	def open(self, name: str, level: str) -> None:
		path = self.file(name)
		if path is None:
			return
		d, err = _open_keep(path, _strictness_of(level))
		if err is not None:
			self.result(err)
			self.diags(err.diags)
			self.doc = new()
			return
		self.doc = d
		self.out("ok")

	def layers(self, specs: list[str]) -> None:
		layers: list[Layer] = []
		for spec in specs:
			if spec.startswith("="):
				layers.append(Layer(text=spec[1:]))
				continue
			path = self.file(spec)
			if path is None:
				return
			layers.append(Layer(file=path))
		try:
			self.doc = open_layers(*layers)
			self.out("ok")
		except Error as e:
			self.doc = new()
			self.result(e)
			self.diags(e.diags)

	def upgraded(self) -> None:
		from_, name = self.doc.upgraded_from(), self.doc.backup_name()
		if not name:
			self.out(f"from {from_} backup none")
			return
		self.out(f"from {from_} backup {_esc(_mask_stamp(os.path.basename(name)))}")
		try:
			self.block(read_text(name))
		except Error as e:
			self.result(e)

	def files(self) -> None:
		if not self.dir:
			self.out("error unsupported")
			return
		try:
			names = sorted(os.listdir(self.dir), key=lambda n: n.encode("utf-8", "surrogateescape"))
		except OSError:
			self.out("error io")
			return
		for n in names:
			self.out(_esc(_mask_stamp(n)))

	def diags(self, ds: list[Diag]) -> None:
		for d in ds:
			line = _spot_text(d.line, d.layer)
			self.out(f"{line}\t{d.severity.value}\t{d.code}\t{_esc(d.message)}")

	def block(self, text: str) -> None:
		"""Multi-line text one line per output line, so a want file diff shows
		which line changed."""
		lines = text.split("\n")
		if lines[-1] == "":
			lines.pop()
		for line in lines:
			self.out("| " + _esc(line))
		if text and not text.endswith("\n"):
			self.out("(no newline at end)")

	def get(self, typ: str, path: str) -> str:
		d = self.doc
		reads: dict[str, tuple[Callable[[str], Got[Any]], Callable[[Any], str]]] = {
			"int": (d.get_int, str),
			"float": (d.get_float, format_float),
			"bool": (d.get_bool, _bool),
			"str": (d.get_str, _esc),
			"raw": (d.get_raw, _esc),
			"strs": (d.get_strs, lambda v: _counted(len(v), "\t".join(_esc(x) for x in v))),
			"ints": (d.get_ints, lambda v: _counted(len(v), "\t".join(map(str, v)))),
			"floats": (d.get_floats, lambda v: _counted(len(v), _join_floats(v))),
		}
		if typ not in reads:
			return "error badtype"
		read, show = reads[typ]
		g = read(path)
		return f"{g.status.name}\t{g.line}\t{show(g.value)}"

	def or_(self, typ: str, path: str, fallback: list[str]) -> None:
		d = self.doc
		first = fallback[0] if fallback else ""
		if typ == "int":
			n = _parse_int(first)
			self.out("error badvalue" if n is None else str(d.int_or(path, n)))
		elif typ == "float":
			x = _parse_float(first)
			self.out("error badvalue" if x is None else format_float(d.float_or(path, x)))
		elif typ == "bool":
			self.out(_bool(d.bool_or(path, first == "true")))
		elif typ == "str":
			self.out(_esc(d.str_or(path, first)))
		elif typ == "raw":
			self.out(_esc(d.raw_or(path, first)))
		elif typ == "strs":
			v = d.strs_or(path, fallback)
			self.out(_counted(len(v), "\t".join(_esc(x) for x in v)))
		elif typ in ("ints", "floats"):
			vals, ok = _parse_value(typ, fallback)
			if not ok:
				self.out("error badvalue")
			elif typ == "ints":
				got = d.ints_or(path, vals)
				self.out(_counted(len(got), "\t".join(map(str, got))))
			else:
				got = d.floats_or(path, vals)
				self.out(_counted(len(got), _join_floats(got)))
		else:
			self.out("error badtype")

	def check(self, name: str, mode: str) -> None:
		path = self.file(name)
		if path is None:
			return
		try:
			text = read_text(path)
		except Error as e:
			self.result(e)
			return
		schema = parse(text)
		ds = self.doc.check(schema)
		if mode in ("all", "drop"):
			ds = self.doc.diags() + ds
		if mode == "drop":
			ds = schema.drop_declared(ds)
		self.diags(ds)

	def upgrade(self, from_text: str) -> None:
		n = 0 if from_text == "" else _parse_int(from_text)
		if n is None:
			self.out("error badvalue")
			return
		try:
			u = upgrade(self.input, n)
		except Error as e:
			self.result(e)
			return
		self.out(f"from {u.from_} changed {_bool(u.changed)} unsure {u.unsure}")
		self.out(f"lost {_line_list(u.lost)} rewritten {_line_list(u.rewritten)}")
		self.block(u.text)

	def scribble(self, name: str, text: str) -> None:
		"""Writes text to a file as another program would, behind the
		document's back."""
		path = self.file(name)
		if path is None:
			return
		try:
			Path(path).write_bytes(text.encode("utf-8", "surrogateescape"))
		except OSError:
			self.out("error io")
			return
		self.out("ok")

	def mode(self, name: str) -> None:
		path = self.file(name)
		if path is None:
			return
		try:
			self.out(f"{stat.S_IMODE(os.stat(path).st_mode) & 0o777:04o}")
		except OSError:
			self.out("error io")

	def write(self, name: str, flags: list[str]) -> None:
		path = self.file(name)
		if path is None:
			return
		force, create, backup, mode = False, False, False, None
		for flag in flags:
			if flag == "force":
				force = True
			elif flag == "create":
				create = True
			elif flag == "backup":
				backup = True
			elif flag.startswith("mode=") and re.fullmatch(r"[0-7]+", flag[5:]):
				mode = int(flag[5:], 8)
			else:
				self.out("error badvalue")
				return
		try:
			self.doc.write(path, force, create=create, backup=backup, mode=mode)
			text = read_text(path)
		except Error as e:
			self.result(e)
			return
		self.out("ok")
		self.block(text)


def _line_list(lines: list[int]) -> str:
	# [3 4], as Go prints a slice
	return "[" + " ".join(map(str, lines)) + "]"


def _bool(b: bool) -> str:
	return "true" if b else "false"
