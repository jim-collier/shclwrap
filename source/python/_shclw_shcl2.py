# SPDX-License-Identifier: MIT
# Copyright © 2026 Jim Collier (CryptogID: ѳ6ᴚ℈𐀘𐇦ɛ𐊁¥Mﾏb϶Δ𐌞)

"""SHCL binding for Python: parser, accessor, writer/formatter.

Single file on purpose - the drop-in story is "copy this file into your tree".
Behavior tracks the Rust reference (source/rust/src/lib.rs) byte for byte; the
conformance corpus in project/conformance/ pins every behavior here, and the
cicd cross-binding check compares this against the reference on every run.
Structure deliberately mirrors the reference over Python idiom, so a fix there
ports here by mechanical diff (parity over idiom - see style-guide.md).
"""

# The type gate, turned on here rather than left at its default: without this the
# checker skips every unannotated body, which in a module with no annotations is
# the whole file - a gate that cannot fail. `assignment` stays off because the
# structures mirrored from the reference are tagged tuples and index-or-None
# locals, which read as type changes to a checker and are not; everything that
# catches a real misuse (a None reaching an operator, a wrong argument, a bad
# return) is live.
# mypy: check-untyped-defs, disable-error-code="assignment"

import math
import os
import stat
import sys
from decimal import Decimal
from enum import Enum

# The public surface, stated rather than inferred: without this `from shcl
# import *` hands out this module's own imports (math, os, stat, Decimal, Enum)
# and its internal ROOT constant alongside the real API.
__all__ = [
	"Diagnostic",
	"Document",
	"FileStatus",
	"LoadError",
	"MAX_DEPTH",
	"Read",
	"SaveError",
	"SaveFailed",
	"SaveRefused",
	"Severity",
	"ShclDateTime",
	"Status",
	"StatusError",
	"Strictness",
	"WriteReason",
	"format_float",
	"generate",
	"parse_datetime",
	"quote_segment",
	"suppress_declared_reopens",
	"suppress_declared_repeats",
	"write_file_atomic",
]

# ---------------------------------------------------------------------------
# Public surface
# ---------------------------------------------------------------------------


class Strictness(Enum):
	Loose = 1
	Standard = 2
	Strict = 3

	@staticmethod
	def from_arg(s):
		"""Accepts the CLI spellings: loose|standard|strict or 1|2|3."""
		return {
			"loose": Strictness.Loose, "1": Strictness.Loose,
			"standard": Strictness.Standard, "2": Strictness.Standard,
			"strict": Strictness.Strict, "3": Strictness.Strict,
		}.get(_ascii_lower(s))


class Severity(Enum):
	# .name renders "Error"/"Hint" - that spelling is the `check` output contract.
	Error = 1
	Hint = 2


class Status(Enum):
	# Values are the CLI exit codes, so status_code is just Status.value.
	Good = 0
	Empty = 2
	NotFound = 3
	BadType = 4
	Multiple = 5


class WriteReason(Enum):
	"""Why a write would fail (write_reason()): the distinctions behind a
	setter's bare False. Writable = the path passes the writer's validation;
	the rest name the five ways it cannot."""
	Writable = 0
	BadPath = 1       # empty path, the scanner rejected it, or a segment carries a line break
	ValueInPath = 2   # the path carries a `: value` part; writes take values separately
	Wildcard = 3      # wildcard selectors are query-only
	NoSuchIndex = 4   # a `[#k]` instance that does not (and can never) exist
	TooDeep = 5       # deeper than the nesting cap; the writer never creates past it


class Diagnostic:
	__slots__ = ("line", "severity", "message", "code")

	def __init__(self, line, severity, message, code):
		self.line = line          # 1-based
		self.severity = severity
		self.message = message
		self.code = code          # stable machine code (E001.., H001..); the contract


# The one place prose couples to a code, so the wording stays free everywhere else.
def _diag_code(msg):
	for prefix, code in (
		("field mixed with list elements", "E001"),
		("value after selector on ", "E002"),
		("no instance ", "E003"),
		("wildcard selector is query-only", "E004"),
		("unterminated raw block", "E005"),
		("raw block with no parent field", "E006"),
		("list element with no parent field", "E007"),
		("list element mixed with field children", "E008"),
		("empty list element", "E009"),
		("bare comma in list element", "E010"),
		("field already has a value", "E011"),
		("indentation matches no open level", "E012"),
		("malformed line skipped", "E014"),
		("malformed line: ", "E013"),
		("missing colon", "E015"),
		("nesting deeper than", "E016"),
		("unterminated quote in value", "E017"),
		("merged with ", "H002"),
		("unknown field ", "V001"),
		("required path missing", "V002"),
		("wrong type at ", "V003"),
		("value not allowed at ", "V004"),
		("value below min at ", "V005"),
		("value above max at ", "V006"),
		("instance count out of bounds at ", "V007"),
		("unknown schema key ", "V090"),
		("unknown schema type ", "V091"),
		("bad schema constraint ", "V092"),
		("bad schema path", "V093"),
		("bad schema fragment", "V094"),
		("unknown schema fragment ", "V095"),
		("schema expands past ", "V096"),
		("schema failed to load", "V099"),
	):
		if msg.startswith(prefix):
			return code
	return "E000"


class Read:
	"""Value plus status plus the original raw text (when the path resolved).
	Array reads also carry one status per slot (element, or wildcard instance)
	in .slots; .status is then the worst slot. Scalar reads leave .slots empty.
	.line is the 1-based source line of the resolved binding (0 when the path
	did not resolve to one node, or the node was writer-built), so a consumer
	check the schema cannot express can still cite the line. .quoted is True
	when the read's single scalar element was quoted in the source - the escape
	hatch that lets a downstream language reserve @null while "@null" stays a
	plain string. Arrays, raw blocks, and empties leave it False."""
	__slots__ = ("value", "status", "raw", "slots", "line", "quoted")

	def __init__(self, value, status, raw, slots=None):
		self.value = value
		self.status = status
		self.raw = raw
		self.slots = slots if slots is not None else []
		self.line = 0
		self.quoted = False

	def _at(self, line, quoted):
		self.line = line
		self.quoted = quoted
		return self

	def ok(self):
		"""Whether the author addressed this field at all: Good or Empty. Note
		this deliberately answers differently from the convenience tier, which
		falls back on Empty like any other non-Good read - ok() asks "is this
		field spoken for", get_*_or() asks "do I have a usable value", and an
		explicitly emptied field is the case where those two diverge."""
		return self.status in (Status.Good, Status.Empty)


class LoadError(Exception):
	"""A failed strict load. Carries the full diagnostics list AND the document
	the parse produced anyway - recover-and-continue means the diagnostics are
	the point, and the tree is what a Standard load would have kept."""
	def __init__(self, diagnostics, document=None):
		self.diagnostics = diagnostics
		self.document = document
		# Name the first few failures right in the message; the bare count made
		# callers dig for information the error was already holding.
		errs = [d for d in diagnostics if d.severity == Severity.Error]
		msg = f"strict load failed: {len(errs)} error diagnostic(s)"
		for d in errs[:3]:
			msg += f"; line {d.line}: {d.code} {d.message}"
		if len(errs) > 3:
			msg += f"; +{len(errs) - 3} more"
		super().__init__(msg)


class SaveError(Exception):
	"""Base for the two ways a save does not happen. They are separate classes
	so a caller can tell them apart with `except` rather than by matching on the
	message: a refusal is theirs to reverse, a write failure is not."""


class SaveRefused(SaveError):
	"""The lost-content gate fired: the load dropped content this save would
	delete (see lost_count). save_file_lossy is the override."""
	def __init__(self, path, lost):
		self.path = path
		self.lost = lost
		super().__init__(f"{path}: refusing to save: load dropped {lost} line(s)/value(s) this write would delete (see diagnostics; save_file_lossy overrides)")


class SaveFailed(SaveError):
	"""The write itself failed; the message is what the system reported."""


class ShclDateTime:
	"""Local (floating) date/time unless a zone suffix was present. Fields mirror
	what was written: a date-only value has no time, and vice versa."""
	__slots__ = ("date", "time", "frac", "zone")

	def __init__(self, date=None, time=None, frac=None, zone=None):
		self.date = date          # None | (year, month, day)
		self.time = time          # None | (hour, minute, seconds-if-written)
		self.frac = frac          # None | fractional-second digits as typed
		self.zone = zone          # None | ("utc", None) | ("offset", minutes)

	def __str__(self):
		out = []
		if self.date is not None:
			y, m, d = self.date
			out.append(f"{y:04d}-{m:02d}-{d:02d}")
			if self.time is not None:
				out.append("T")
		if self.time is not None:
			h, mi, s = self.time
			out.append(f"{h:02d}:{mi:02d}")
			if s is not None:
				out.append(f":{s:02d}")
			if self.frac is not None:
				out.append("." + self.frac)
		if self.zone is not None:
			if self.zone[0] == "utc":
				out.append("Z")
			else:
				off = self.zone[1]
				sign = "-" if off < 0 else "+"
				a = abs(off)
				out.append(f"{sign}{a // 60:02d}:{a % 60:02d}")
		return "".join(out)


def format_float(v):
	"""Float -> string, matching the reference: positional, shortest round-trip,
	never scientific. inf/NaN spelled as the reference spells them."""
	if v != v:
		return "NaN"
	if v == math.inf:
		return "inf"
	if v == -math.inf:
		return "-inf"
	return format(Decimal(repr(v)).normalize(), "f")


# ---------------------------------------------------------------------------
# In-memory model
# ---------------------------------------------------------------------------
# One rule covers everything: a node is (field-name, value, children); nodes
# merge when (name, value) matches; empty values merge into the wrapper node.


class _Element:
	__slots__ = ("text", "quoted")   # text: quote-stripped, escapes NOT applied (names differ - see _scan_path_ex)
	# Declared, not assigned - __slots__ forbids class attributes. The point is
	# the type gate: without a type here every field is Any and the checker has
	# nothing to check.
	text: str
	quoted: bool

	def __init__(self, text, quoted):
		self.text = text
		self.quoted = quoted


class _Lead:
	"""One whole-line comment held as trivia, plus whether a blank line preceded
	it - so a blank between comment-only regions survives the round-trip
	(blank runs collapse to one, same as nodes)."""
	__slots__ = ("text", "blank_before")

	def __init__(self, text, blank_before):
		self.text = text
		self.blank_before = blank_before


class _Pend:
	"""A pending whole-line comment during parse: text, source indent (used only
	to decide whether it hangs on a deeper block), and the blank it consumed."""
	__slots__ = ("text", "indent", "blank_before")

	def __init__(self, text, indent, blank_before):
		self.text = text
		self.indent = indent
		self.blank_before = blank_before


# Shared empty element list for the kinds that have none: only "cell" ever
# mutates els (the stacked-list append checks kind first), so "empty" and
# "raw" values all point at this one tuple instead of a list each.
_EMPTY_ELS: tuple = ()


class _Value:
	# kind: "empty" | "cell" (els) | "raw" (content/info/fence_char/fence_len)
	__slots__ = ("kind", "els", "content", "info", "fence_char", "fence_len")
	kind: str
	els: list     # _EMPTY_ELS when the kind has no elements; list only for "cell"
	content: str
	info: str
	fence_char: str
	fence_len: int

	def __init__(self, kind):
		# Empty rather than None for the three the kind decides: the fields a kind
		# does not use are never read, and starting them at their own empty value
		# keeps every reader's type honest without a guard at each use.
		self.kind = kind
		self.els = _EMPTY_ELS
		self.content = ""
		self.info = ""
		self.fence_char = None
		self.fence_len = None

	def is_empty(self):
		return self.kind == "empty"

	def copy(self):
		"""Independent copy, element list included - a clone or a merged-in value
		has to survive the document it came from being released."""
		v = _Value(self.kind)
		if self.kind == "cell":
			v.els = [_Element(e.text, e.quoted) for e in self.els]
		v.content = self.content
		v.info = self.info
		v.fence_char = self.fence_char
		v.fence_len = self.fence_len
		return v

	def key(self):
		"""Merge key: nodes with equal (name, key) collapse into one."""
		if self.kind == "empty":
			return "e"
		if self.kind == "cell":
			# Length-prefix each element so the joined key is injective: a bare NUL
			# separator lets `[a, b]` collide with the single element "a\0b" (NUL is
			# legal in a quoted string), silently merging them.
			parts = ["c:"]
			for e in self.els:
				parts.append(str(len(e.text)))
				parts.append(":")
				parts.append(e.text)
			return "".join(parts)
		# Info-string is part of identity (a `sql` and a `python` block are
		# different values even with equal bodies); fence style is not. Info is
		# length-prefixed for the same injectivity reason as cell elements.
		return "r:" + str(len(self.info)) + ":" + self.info + self.content

	def display(self):
		"""Human/display form; also what selectors match against (case-sensitive)."""
		if self.kind == "empty":
			return ""
		if self.kind == "cell":
			return ", ".join(e.text for e in self.els)
		return self.content


def _empty():
	return _Value("empty")


def _cell(els):
	v = _Value("cell")
	v.els = els
	return v


def _raw(content, info, fence_char, fence_len):
	v = _Value("raw")
	v.content = content
	v.info = info
	v.fence_char = fence_char
	v.fence_len = fence_len
	return v


def _literal_value(text):
	# Read text as the value half of a line, for the setters that take value
	# syntax rather than data. Rejects what could not have come off one line: a
	# line break, or a quote that never closes. An unquoted # ends the value
	# here exactly as it would in a file.
	if "\n" in text or "\r" in text:
		return None
	v = _trim(_split_comment(text)[0])
	if _unterminated_quote(v):
		return None
	return _parse_cell(v)


def _fits_i64(v):
	# Python's int is unbounded, the other three bindings' are 64-bit. Text for a
	# value outside that range reads back bad-type in every binding, python
	# included, so a setter refuses it rather than writing a value nothing can
	# read. Same range-check the CLI already does on its side.
	return -(1 << 63) <= v <= (1 << 63) - 1


def _cell_of(text):
	return _cell([_Element(text, False)])


def _array_cell(texts):
	# Inline-array value; the empty array is an empty value (reads back Empty).
	if not texts:
		return _empty()
	return _cell([_Element(t, False) for t in texts])


def _encode_string(s):
	"""Inverse of a scalar string read (_apply_escapes): only backslash, newline,
	and tab need encoding; _emit_element wraps quote/reserved chars itself and
	reparse strips that wrapping."""
	out = []
	for c in s:
		if c == "\\":
			out.append("\\\\")
		elif c == "\n":
			out.append("\\n")
		elif c == "\t":
			out.append("\\t")
		else:
			out.append(c)
	return "".join(out)


def _choose_fence(content):
	"""Pick a backtick fence long enough that no content line closes it early."""
	maxrun = 0
	for line in content.split("\n"):
		t = _trim(line)
		if t and all(ch == "`" for ch in t):
			maxrun = max(maxrun, len(t))
	return ("`", max(3, maxrun + 1))


class _Trivia:
	"""Comment trivia, boxed off to the side: most nodes carry none, and the
	four empty containers were a third of every node. Verbatim from `#` to end
	of line. Never part of identity or reads; merged instances concatenate
	leading, first trailing wins (later ones demote to leading - a canonical
	line has room for one)."""
	__slots__ = ("leading", "trailing", "after", "inside")

	def __init__(self):
		self.leading = []
		self.trailing = ""        # empty = none
		# Whole-line comments that followed this node's subtree at a deeper
		# indent than the next binding - they belong to this block, not the next
		# node, so a run trailing a block's last child stays put instead of
		# re-attaching dedented. Emitted after the subtree at this node's depth.
		self.after = []
		# Whole-line comments written inside this node's block when no bound
		# child could take them - a header whose children are all commented
		# still owns those lines. Emitted after the subtree one level deeper
		# than this node.
		self.inside = []


class _Node:
	__slots__ = (
		"name", "value", "children", "parent", "line", "star_list", "star_mixed",
		"trivia", "blank_before", "src_set", "src", "name_src",
	)

	def __init__(self, name, value, parent, line, name_src=""):
		# ASCII-folded to lower; non-ASCII never folds. Interned: siblings and
		# repeated sections share one string object instead of one per node.
		self.name = sys.intern(name)
		# The name as the author spelled it (case unfolded, quotes and escapes
		# resolved) - what authored_name() hands back. Merged instances keep the
		# first binding's spelling, like line() and comments. The same object as
		# `name` when the spellings match (the overwhelmingly common case), so
		# the duplicate per-node string never allocates.
		self.name_src = self.name if name_src == name else sys.intern(name_src)
		self.value = value
		self.children = []
		self.parent = parent
		self.line = line
		self.star_list = False    # value built from stacked "* " lines
		self.star_mixed = False   # mix of "* " and field children already diagnosed
		# Comment trivia sidecar (_Trivia): None until the first write, so the
		# common comment-free node never allocates the four containers.
		self.trivia = None
		# Blank-line grouping is the other half of hand-authored layout: set
		# when a blank line preceded this node's binding line (runs collapse).
		self.blank_before = False
		# This node has decided its `src` - set by the first source line whose
		# value the node holds, whether or not a string was worth keeping.
		self.src_set = False
		# Verbatim value text from the source line (after the colon, comment
		# stripped, trimmed) - what a read's `raw` hands back. None when the
		# value was synthesized (writer, stacked list, fence) OR when the
		# spelling is exactly the display form; raw falls back to the display
		# form either way.
		self.src = None

	# Trivia reads hand back the empty shape when no sidecar exists; _triv()
	# allocates one on first write.
	def leading(self):
		t = self.trivia
		return t.leading if t is not None else ()

	def trailing(self):
		t = self.trivia
		return t.trailing if t is not None else ""

	def after(self):
		t = self.trivia
		return t.after if t is not None else ()

	def inside(self):
		t = self.trivia
		return t.inside if t is not None else ()

	def _triv(self):
		t = self.trivia
		if t is None:
			t = self.trivia = _Trivia()
		return t


ROOT = 0


def _fold_node_into(arena, survivor, loser):
	"""Merge a later instance into an earlier one under the in-file merge rule:
	children and trivia move over, first trailing wins (a second demotes to a
	leading line), first spelling stays. The caller drops the loser from the
	parent's child list; it keeps its arena slot, unreferenced."""
	# Lists are references: after each move, point the loser at a fresh empty
	# list so the two nodes never share one.
	kids = arena[loser].children
	arena[loser].children = []
	for k in kids:
		arena[k].parent = survivor
	arena[survivor].children.extend(kids)
	lt = arena[loser].trivia
	if lt is not None:
		arena[loser].trivia = None
		st = arena[survivor]._triv()
		st.leading.extend(lt.leading)
		if lt.trailing:
			if not st.trailing:
				st.trailing = lt.trailing
			else:
				st.leading.append(_Lead(lt.trailing, False))
		st.after.extend(lt.after)
		st.inside.extend(lt.inside)


# Maximum nesting depth (levels below the document root), enforced at load and
# by the Writer. Deeper lines are skipped with an E016 error. The cap is what
# keeps the recursive tree walks (emit, merge, clone) safely inside every
# binding's stack, so a hostile or machine-generated document can make a load
# fail but never crash the consumer.
MAX_DEPTH = 512


# ---------------------------------------------------------------------------
# Lexical helpers
# ---------------------------------------------------------------------------

# White_Space set, matching Rust char::is_whitespace exactly (so trims agree on
# exotic input - e.g. U+001C-1F are NOT whitespace here, unlike Python's default).
#
# The characters are written out rather than escaped, which is why the noqa is
# here: a linter reads them as typos for a plain space. Do not "fix" them, and do
# not add or drop one without changing the C, Go and Rust sets in the same pass -
# the four have to agree or the bindings stop trimming alike.
_WS = (
	"\t\n\x0b\x0c\r\x20\x85\xa0 "  # noqa: RUF001
	"           "  # noqa: RUF001
	"    　"  # noqa: RUF001
)
_WS_SET = set(_WS)

_ASCII_UPPER_MAP = {ord(c): ord(c) + 32 for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ"}


def _ascii_lower(s):
	# Folds A-Z only; non-ASCII passes through untouched (matches to_ascii_lowercase).
	return s.translate(_ASCII_UPPER_MAP)


def _trim(s):
	return s.strip(_WS)


def _trim_end(s):
	return s.rstrip(_WS)


def _split_ws(s):
	# Like Rust split_whitespace: split on White_Space runs, no empty tokens.
	out = []
	cur: list = []
	for c in s:
		if c in _WS_SET:
			if cur:
				out.append("".join(cur))
				cur = []
		else:
			cur.append(c)
	if cur:
		out.append("".join(cur))
	return out


def _is_ascii_digit(c):
	return "0" <= c <= "9"


def _fold_name(s):
	return _ascii_lower(s)


# A closed 64-character set, so membership is one C-level lookup rather than
# two method calls and two comparisons per character. The scanner and the name
# emitter both run this over every character they touch.
_BARE_NAME_CHARS = frozenset(
	"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_"
)


def _is_bare_name_char(c):
	return c in _BARE_NAME_CHARS


def _split_comment(s):
	"""Split off an unquoted trailing comment: (content, comment from `#` on,
	"" = none). A `\\` shields the next char throughout. Comments are kept as
	trivia."""
	# Fast path: the loop's quote/backslash state only decides whether a `#`
	# counts, and its lone early return fires on `#` - so with no `#` at all the
	# answer is (s, "") whatever the state. `in` scans at C speed; the char loop
	# below is the cost, and comment-free lines dominate real documents.
	if "#" not in s:
		return s, ""
	in_quote = None
	i = 0
	n = len(s)
	while i < n:
		c = s[i]
		if c == "\\":
			i += 2
			continue
		if in_quote is not None:
			if c == in_quote:
				in_quote = None
		elif c == '"' or c == "'":
			in_quote = c
		elif c == "#":
			return s[:i], s[i:]
		i += 1
	return s, ""


def _split_unquoted_commas(s):
	"""Split on unquoted commas; `\\` shields the next char."""
	# Fast paths: parts are cut only at commas, so with no comma the result is
	# [s] no matter what the quote/backslash state did (a shield can only make
	# a comma NOT split, never conjure one). And with no quote or backslash
	# every comma splits, which is exactly str.split.
	if "," not in s:
		return [s]
	if '"' not in s and "'" not in s and "\\" not in s:
		return s.split(",")
	parts = []
	in_quote = None
	start = 0
	i = 0
	n = len(s)
	while i < n:
		c = s[i]
		if c == "\\":
			i += 2
			continue
		if in_quote is not None:
			if c == in_quote:
				in_quote = None
		elif c == '"' or c == "'":
			in_quote = c
		elif c == ",":
			parts.append(s[start:i])
			start = i + 1
		i += 1
	parts.append(s[start:])
	return parts


def _normalize_dangling_backslash(t):
	"""A dangling trailing backslash would swallow the following separator on
	re-emit; store the doubled spelling instead (identical on string read)."""
	run = 0
	for c in reversed(t):
		if c == "\\":
			run += 1
		else:
			break
	if run % 2 == 1:
		t += "\\"
	return t


def _unterminated_quote(text):
	"""True when some piece starts with a quote that never closes (missing or
	escaped). Such a piece stays literal - and the quote-aware comment strip has
	already swallowed any trailing # comment into it - so the parser calls it
	out instead of letting the typo look deliberate. Mid-text apostrophes
	(it's fine) are legal prose and stay silent."""
	for piece in _split_unquoted_commas(text):
		t = _trim(piece)
		if not t:
			continue
		first = t[0]
		if first != '"' and first != "'":
			continue
		closed = False
		if len(t) >= 2 and t[-1] == first:
			esc = False
			for c in t[1:-1]:
				esc = (c == "\\") and not esc
			closed = not esc
		if not closed:
			return True
	return False


def _parse_element(piece):
	"""Trim, then strip one matching outer quote pair if present. Unquoted empty
	slots return None (dropped, never an error)."""
	t = _trim(piece)
	if not t:
		return None
	first = t[0]
	if (first == '"' or first == "'") and len(t) >= 2 and t[-1] == first:
		# The closing quote must not itself be escaped (`"a\"` is not closed).
		esc = False
		for c in t[1:-1]:
			esc = (c == "\\") and not esc
		if not esc:
			return _Element(t[1:-1], True)
	return _Element(_normalize_dangling_backslash(t), False)


def _parse_cell(text):
	els = []
	for piece in _split_unquoted_commas(text):
		e = _parse_element(piece)
		if e is not None:
			els.append(e)
	return _cell(els) if els else _empty()


def _apply_escapes(s):
	"""Escape processing (string reads): \\t \\n \\\\ \\" \\'; unknown escapes stay literal."""
	# Fast path: every non-backslash char passes through verbatim, so with no
	# backslash the output is s itself. Hot at parse time too (_disp_key runs
	# per node insert), and backslash-free text dominates.
	if "\\" not in s:
		return s
	out = []
	it = iter(s)
	for c in it:
		if c != "\\":
			out.append(c)
			continue
		nxt = next(it, None)
		if nxt == "t":
			out.append("\t")
		elif nxt == "n":
			out.append("\n")
		elif nxt == "\\":
			out.append("\\")
		elif nxt == '"':
			out.append('"')
		elif nxt == "'":
			out.append("'")
		elif nxt is None:
			out.append("\\")
		else:
			out.append("\\")
			out.append(nxt)
	return "".join(out)


def _single_scalar(v):
	"""The restriction a QUOTED [value] selector adds on top of the display
	match: quoting selects the scalar spelling only, so the scalar "a, b" and
	the list a, b stop meeting the same selector."""
	return v.kind == "cell" and v.els is not None and len(v.els) == 1


def _disp_key(v):
	"""The predicate a `[value]` selector matches with: display form with escapes
	applied on both sides, so `["q\\"uote"]` finds `'q"uote'` - a logical-string
	match, not spelling against spelling."""
	return _apply_escapes(v.display())


def _merge_key(name, v):
	"""The (name, merge-key) accelerator key, as an exact tuple reusing the
	value's own strings: injective exactly like name plus _Value.key(), with no
	key text built or copied. Where the reference streams these fields through
	an FNV hash and verifies hits, tuples keep the lookup exact - dict and
	tuple machinery here is C-speed, a hand-rolled hash loop is not."""
	k = v.kind
	if k == "cell":
		return (name, "c", tuple(e.text for e in v.els))
	if k == "empty":
		return (name, "e")
	return (name, "r", v.info, v.content)


def _value_key(v):
	"""The value's merge key alone (no name part) - the src-attach guard's
	compare."""
	return _merge_key("", v)


def _src_matches_display(v, s):
	"""True when a source value spelling is exactly the display form - the case
	where `src` need not be stored, since raw's fallback reproduces it. The
	single-scalar display() is the element text itself, so the common compare
	builds nothing."""
	return s == v.display()


def _fence_open(rest):
	"""Opening fence: a run of >=3 backticks or tildes, then an optional info-string."""
	if not rest:
		return None
	first = rest[0]
	if first != "`" and first != "~":
		return None
	run = 0
	for c in rest:
		if c == first:
			run += 1
		else:
			break
	if run < 3:
		return None
	return (first, run, _trim(rest[run:]))


def _is_fence_close(line, ch, min_len):
	t = _trim(line)
	return len(t) >= min_len and len(t) > 0 and all(c == ch for c in t)


def _strip_common(line, common):
	"""Remove a raw block's common indent from one content line.

	A whitespace-only line took no part in computing that indent, so it can be
	shorter than it - strip only what it actually shares, rather than blanking
	it. Blanking drops author spacing on a line the spec calls verbatim.
	"""
	k = 0
	while k < len(common) and k < len(line) and common[k] == line[k]:
		k += 1
	return line[k:]


# ---------------------------------------------------------------------------
# Path scanner (shared by file lines and accessor queries)
# ---------------------------------------------------------------------------
# Selector is a tuple: ("val", str, quoted) | ("idx", int) | ("wild", None).
# quoted: the selector text was quoted in the path. A quoted selector is
# scalar-only - it matches a single-element value whose logical string equals
# the text - so quoting distinguishes the scalar "a, b" from the two-element
# list a, b, the same way quoting escapes elsewhere.


class _Segment:
	# name folded, name_src as authored (unfolded, quotes stripped, escapes
	# applied); selector None or tuple; star = bare `*` name wildcard
	# (quoted "*" stays a literal name).
	__slots__ = ("name", "name_src", "selector", "star")

	def __init__(self, name, name_src, selector, star=False):
		self.name = name
		self.name_src = name_src
		self.selector = selector
		self.star = star


class _PathError(Exception):
	pass


def _parse_uint(s):
	# Rust usize::from_str: an optional leading '+', then ASCII digits, no underscores.
	if not s:
		return None
	body = s[1:] if s[0] == "+" else s
	if not body or not all(_is_ascii_digit(c) for c in body):
		return None
	# Length-gate before int(): CPython 3.11+ refuses >4300 decimal digits, but the
	# reference just overflows. Leading zeros are legal and don't count toward range.
	digits = body.lstrip("0") or "0"
	if len(digits) > 20:
		return None
	n = int(digits)
	if n > 2 ** 64 - 1:
		return None
	return n


def _scan_path(inp):
	"""Scan `a . b : [sel] . c : value`. Whitespace around dots/colons/brackets is
	insignificant. A colon is a selector colon only when the next non-ws char is
	`[`; otherwise it separates the value. Raises _PathError on genuinely ambiguous
	input, which the caller skips with a diagnostic. Returns (segments, value_text)
	where value_text is None (no colon) or the trimmed text after the colon."""
	return _scan_path_ex(inp, False)


def _scan_lookup(inp):
	"""Query spelling of _scan_path: also accepts a bare `*` segment (the name
	wildcard - any child name). Document lines never take it; only lookups
	(reads, the writer probe, schema paths) do."""
	return _scan_path_ex(inp, True)


def _scan_path_ex(inp, stars):
	chars = inp
	n = len(chars)

	def skip_ws(p):
		while p < n and (chars[p] == " " or chars[p] == "\t"):
			p += 1
		return p

	def read_quoted(p):
		q = chars[p]
		p += 1
		out = []
		while True:
			if p >= n:
				raise _PathError("unterminated quote")
			c = chars[p]
			if c == "\\" and p + 1 < n:
				out.append(c)
				out.append(chars[p + 1])
				p += 2
				continue
			p += 1
			if c == q:
				return "".join(out), p
			out.append(c)

	pos = 0
	segments = []
	while True:
		pos = skip_ws(pos)
		if pos >= n:
			raise _PathError("empty path")
		# Field name: quoted, bare, or (lookups only) the `*` name wildcard.
		star = False
		if chars[pos] == '"' or chars[pos] == "'":
			name, pos = read_quoted(pos)
		elif stars and chars[pos] == "*":
			pos += 1
			star = True
			name = "*"
		else:
			start = pos
			while pos < n and chars[pos] in _BARE_NAME_CHARS:
				pos += 1
			if pos == start:
				raise _PathError(f"expected field name, found '{chars[pos]}'")
			name = chars[start:pos]
		selector = None
		pos = skip_ws(pos)
		# Optional selector, with its optional sugar colon (colon counts as
		# selector sugar only when the next non-ws char is an open bracket).
		bracket_at = None
		if pos < n and chars[pos] == "[":
			bracket_at = pos
		elif pos < n and chars[pos] == ":":
			q = skip_ws(pos + 1)
			if q < n and chars[q] == "[":
				bracket_at = q
		if bracket_at is not None:
			pos = skip_ws(bracket_at + 1)
			if pos < n and (chars[pos] == '"' or chars[pos] == "'"):
				v, pos = read_quoted(pos)
				selector = ("val", v, True)   # quotes force a value match, even numeric - and scalar-only
			else:
				start = pos
				while pos < n and chars[pos] != "]":
					pos += 1
				body = _trim(chars[start:pos])
				if body == "*":
					selector = ("wild", None)
				elif body.startswith("#") and _parse_uint(body[1:]) is not None:
					selector = ("idx", _parse_uint(body[1:]))
				elif _parse_uint(body) is not None:
					selector = ("idx", _parse_uint(body))
				elif body == "":
					raise _PathError("empty selector")
				else:
					selector = ("val", _normalize_dangling_backslash(body), False)
			pos = skip_ws(pos)
			if pos >= n or chars[pos] != "]":
				raise _PathError("unterminated selector")
			pos = skip_ws(pos + 1)
		if star and selector is not None:
			raise _PathError("selector on a name wildcard")
		# Names resolve escapes, the same rule values follow when they are
		# compared: two spellings of one name are one name. name_src keeps the
		# source spelling, which is what authored_name hands back.
		segments.append(_Segment(_fold_name(_apply_escapes(name)), name, selector, star))
		if pos >= n:
			return segments, None
		c = chars[pos]
		if c == ".":
			pos += 1
		elif c == ":":
			pos += 1
			return segments, _trim(chars[pos:])
		else:
			raise _PathError(f"unexpected '{c}' after field")


# ---------------------------------------------------------------------------
# Parser
# ---------------------------------------------------------------------------


class _Parser:
	def __init__(self):
		self.arena = [_Node("", _empty(), 0, 0)]
		self.diags = []
		# (indent string, node) for each open level; [0] is the virtual root.
		self.stack = [("", ROOT)]
		# Per-node (name, value-key) -> first matching child, parallel to arena
		# and None until a parent's first insert, so leaves never allocate one.
		# Pure lookup accelerator for _select_or_create; children keeps the
		# order. Keys are _merge_key tuples, so no key text is built or stored.
		self.child_map: list = [None]
		# Per-node (name, display) -> first matching child: the `[value]` selector
		# accelerator (its predicate is display(), a different and non-injective
		# key from child_map's). Same first-wins discipline, same mutation sites,
		# same lazy allocation.
		self.disp_map: list = [None]
		# Whole-line comments waiting for the next line that binds a node. The
		# source indent is kept only to decide after-attachment (a comment
		# deeper than the next binding hangs on the block it sits in).
		self.pending = []
		self.saw_blank = False  # a blank line waits to become the next bound node's blank_before
		# An open stacked list defers its merge-key remap (rebuilding the key per
		# element is O(list^2) time); (node, map key, display key) at deferral
		# start, flushed before any map lookup and at end of parse.
		self.star_open = None
		# Node -> line of the re-open that H002-hinted it. A merge under a hinted
		# container combines the same two textual regions, so it hints too even
		# when it lands on the newest child at its own scope - that is how every
		# merged level reports, not just the outermost. The stored line splits old
		# children (hint) from ones the re-opened region itself created (silent).
		self.reentered = {}
		# Dropped lines/values canonical output cannot re-emit.
		self.lost = 0

	def _err(self, line, msg):
		self.diags.append(Diagnostic(line, Severity.Error, msg, _diag_code(msg)))

	def _select_or_create(self, parent, name, name_src, value, line):
		"""Find (or create by merge rule) the child of `parent` with this (name, value)."""
		self._star_flush()
		map_key = _merge_key(name, value)
		cmap = self.child_map[parent]
		if cmap is not None:
			found = cmap.get(map_key)
			if found is not None:
				return found
		idx = len(self.arena)
		node = _Node(name, value, parent, line, name_src)
		self.arena.append(node)
		self.arena[parent].children.append(idx)
		self.child_map.append(None)
		self.disp_map.append(None)
		if cmap is None:
			cmap = self.child_map[parent] = {}
		cmap[map_key] = idx
		dmap = self.disp_map[parent]
		if dmap is None:
			dmap = self.disp_map[parent] = {}
		dmap.setdefault((name, _disp_key(node.value)), idx)
		return idx

	def _star_flush(self):
		"""Apply an open stacked list's deferred remap. Runs before any map lookup
		(and at end of parse), so both maps are always fresh when queried."""
		if self.star_open is not None:
			node, key, disp = self.star_open
			self.star_open = None
			self._remap_child(node, key, disp)

	def _remap_child(self, node, old_key, old_disp):
		"""A node's value mutated in place (empty field filled, star element added):
		move its map entry from the old key to the new one. First-wins on both
		sides so lookups keep matching the earliest sibling, like the scan did."""
		parent = self.arena[node].parent
		name = self.arena[node].name
		cmap = self.child_map[parent]
		if cmap is None:
			cmap = self.child_map[parent] = {}
		if cmap.get(old_key) == node:
			del cmap[old_key]
		cmap.setdefault(_merge_key(name, self.arena[node].value), node)
		dmap = self.disp_map[parent]
		if dmap is None:
			dmap = self.disp_map[parent] = {}
		if dmap.get(old_disp) == node:
			del dmap[old_disp]
		dmap.setdefault((name, _disp_key(self.arena[node].value)), node)

	def _fold_late_dups(self):
		"""A value that mutates after its sibling group was keyed - an empty field
		filled by a fence, a stacked list closed - can land on a key an earlier
		sibling already holds, which the keyed lookup can no longer catch. Fold
		those pairs so the tree matches a reparse of its own canonical text.
		Depth-first, since folding can carry duplicates down a level."""
		# Explicit stack: parse-side walks stay iterative so depth can't blow
		# Python's recursion limit.
		stack = [ROOT]
		while stack:
			parent = stack.pop()
			kids = self.arena[parent].children
			first: dict = {}
			keep = []
			for c in kids:
				key = _merge_key(self.arena[c].name, self.arena[c].value)
				survivor = first.get(key)
				if survivor is not None:
					_fold_node_into(self.arena, survivor, c)
				else:
					first[key] = c
					keep.append(c)
			stack.extend(keep)
			self.arena[parent].children = keep

	def _attach_trivia(self, node, trailing):
		"""Hand pending leading comments (and this line's trailing one) to a node.
		First trailing wins; a later one demotes to leading so nothing is lost."""
		if self.pending:
			t = self.arena[node]._triv()
			for p in self.pending:
				t.leading.append(_Lead(p.text, p.blank_before))
			self.pending = []
		if trailing:
			t = self.arena[node]._triv()
			if not t.trailing:
				t.trailing = trailing
			else:
				t.leading.append(_Lead(trailing, False))

	def _hang_deeper_pending(self, new_indent):
		"""Comments written deeper than the incoming line belong to the block
		they sit in, not to the next binding: hang each on the deepest node
		whose bound indent prefixes the comment's, among the levels the
		incoming line is closing. Written at that node's own level the comment
		trails it (`after`); written deeper it sits inside the node's block
		(`inside`) - so a header whose children are all commented still owns
		them at their depth. Runs before the incoming line resolves (and at
		end of parse with the empty indent, so tail comments keep their block)."""
		if not self.pending:
			return
		taken = self.pending
		self.pending = []
		for p in taken:
			if len(p.indent) > len(new_indent):
				# A level shallower than the incoming line stays open and may
				# still gain children, so a comment must not hang there - it
				# would emit below the child; keep it pending instead.
				target = None
				at_own_level = False
				for ind, node in reversed(self.stack):
					if node != ROOT and len(ind) >= len(new_indent) and p.indent.startswith(ind):
						target = node
						at_own_level = len(ind) == len(p.indent)
						break
				if target is not None:
					lead = _Lead(p.text, p.blank_before)
					if at_own_level:
						self.arena[target]._triv().after.append(lead)
					else:
						self.arena[target]._triv().inside.append(lead)
					continue
			self.pending.append(p)

	def _resolve_parent(self, indent):
		"""Resolve which open level this indent belongs to. Child only when the
		current top's indent is a proper prefix; otherwise the indent must equal
		an open level exactly (dedent), else it is a recoverable error."""
		top_indent, top_node = self.stack[-1]
		if len(indent) > len(top_indent) and indent.startswith(top_indent):
			return top_node
		for i in range(len(self.stack) - 1, -1, -1):
			if self.stack[i][0] == indent:
				# Sibling of stack[i]: its parent is the entry below it.
				parent = ROOT if i == 0 else self.stack[i - 1][1]
				self.stack = self.stack[:max(i, 1)]
				if i == 0:
					self.stack = self.stack[:1]
				return parent
		return None

	def _attach_path(self, parent, segs, value, line):
		"""Walk path segments under `parent`, select-or-creating; returns the node
		for the last segment carrying `value`. None aborts the line (diagnosed)."""
		self._star_flush()
		# Field child under a stacked list: diagnose the mix once, keep the field.
		pnode = self.arena[parent]
		if pnode.star_list and not pnode.star_mixed:
			pnode.star_mixed = True
			self._err(line, "field mixed with list elements")
		# Nesting cap: parent depth plus the segments this line adds. Checked
		# before any node is created so a rejected line leaves nothing behind.
		parent_depth = 0
		up = parent
		while up != ROOT:
			parent_depth += 1
			up = self.arena[up].parent
		if parent_depth + len(segs) > MAX_DEPTH:
			self._err(line, f"nesting deeper than {MAX_DEPTH} levels; line skipped")
			self.lost += 1
			return None
		cur = parent
		last = len(segs) - 1
		for i, seg in enumerate(segs):
			is_last = i == last
			sel = seg.selector
			if sel is not None and sel[0] == "val":
				# Same escape-applied display predicate resolution uses, so a
				# selector also selects an array-valued instance instead of
				# creating a spurious second one - via the disp_map accelerator
				# (the inline spelling was quadratic in siblings without it).
				# Create only when nothing matches.
				# A quoted selector is scalar-only, and the accelerator keeps
				# just the first same-display child - a later remap can drop an
				# entry a different sibling still satisfies - so a non-scalar hit
				# and an outright miss both fall to the (rare) fallback scan.
				want = _apply_escapes(sel[1])
				dmap = self.disp_map[cur]
				found = dmap.get((seg.name, want)) if dmap is not None else None
				if found is not None and sel[2] and not _single_scalar(self.arena[found].value):
					found = None
				if found is None and sel[2]:
					for c in self.arena[cur].children:
						nd = self.arena[c]
						if nd.name == seg.name and _single_scalar(nd.value) and _disp_key(nd.value) == want:
							found = c
							break
				if found is not None:
					cur = found
				else:
					disc = _cell([_Element(sel[1], False)])
					cur = self._select_or_create(cur, seg.name, seg.name_src, disc, line)
				if is_last and not value.is_empty():
					# `a.b[X]: v` - the discriminator is the value; a second
					# value has nowhere unambiguous to go.
					self._err(line, f"value after selector on '{seg.name}' ignored")
					self.lost += 1
			elif sel is not None and sel[0] == "idx":
				matches = [c for c in self.arena[cur].children if self.arena[c].name == seg.name]
				k = sel[1]
				if k < len(matches):
					cur = matches[k]
				else:
					self._err(line, f"no instance {k} of '{seg.name}'")
					self.lost += 1
					return None
			elif sel is not None and sel[0] == "wild":
				self._err(line, "wildcard selector is query-only")
				self.lost += 1
				return None
			elif not is_last:
				cur = self._select_or_create(cur, seg.name, seg.name_src, _empty(), line)
			else:
				parent = cur
				before = len(self.arena)
				cur = self._select_or_create(cur, seg.name, seg.name_src, value, line)
				# Two separately-written bindings just combined: legal (the
				# merge rule), but only the parser can see it happened, so
				# say so. Adjacent re-mentions (still the newest binding at
				# this scope) and selector/path-intermediate merges stay
				# silent - those are the deliberate redundant-path idiom.
				# Under a hinted container the newest-child pass does not
				# apply to children the earlier region wrote: those merges
				# combine the same two regions, so every level reports.
				if cur < before and self.arena[cur].line != line:
					non_last = self.arena[parent].children[-1] != cur
					reopen_line = self.reentered.get(parent)
					cross_region = reopen_line is not None and self.arena[cur].line < reopen_line
					if non_last or cross_region:
						at = self.arena[cur].line
						self.diags.append(Diagnostic(
							line, Severity.Hint,
							f"{_h002_head(seg.name)}line {at} (same name and value combine)",
							"H002"))
						self.reentered[cur] = line
		return cur

	def _consume_raw(self, lines, i, open_line, ch, length, info):
		"""Consume raw-block content after an opening fence. Returns (value, next
		line index). Content keeps relative indentation; the common leading run
		is stripped."""
		content = []
		closed = False
		while i < len(lines):
			if _is_fence_close(lines[i], ch, length):
				closed = True
				i += 1
				break
			content.append(lines[i])
			i += 1
		if not closed:
			self._err(open_line, "unterminated raw block")
		# Strip the common leading whitespace (the visual nesting); keep the rest.
		common = None
		for ln in content:
			if not _trim(ln):
				continue
			lead_chars: list = []
			for c in ln:
				if c == " " or c == "\t":
					lead_chars.append(c)
				else:
					break
			lead = "".join(lead_chars)
			if common is None:
				common = lead
			else:
				p = []
				for a, b in zip(common, lead):
					if a == b:
						p.append(a)
					else:
						break
				common = "".join(p)
		if common is None:
			common = ""
		stripped = [_strip_common(ln, common) for ln in content]
		return _raw("\n".join(stripped), info, ch, length), i

	def _bind_block(self, parent, value, line):
		"""A bare fence line is a value line for its parent field: fills an empty
		value, else creates a new instance of that field (the repeated-leaf rule).
		Returns the node the block landed on (None = no parent, diagnosed)."""
		if parent == ROOT:
			self._err(line, "raw block with no parent field")
			self.lost += 1
			return None
		if self.arena[parent].value.is_empty():
			pnode = self.arena[parent]
			old_key = _merge_key(pnode.name, pnode.value)
			old_disp = (pnode.name, _disp_key(pnode.value))
			pnode.value = value
			self._remap_child(parent, old_key, old_disp)
			return parent
		name = self.arena[parent].name
		name_src = self.arena[parent].name_src
		grandparent = self.arena[parent].parent
		return self._select_or_create(grandparent, name, name_src, value, line)

	def _add_star_element(self, parent, body, line):
		"""One stacked-list element (`* scalar`) appends to the parent's array."""
		if parent == ROOT:
			self._err(line, "list element with no parent field")
			self.lost += 1
			return
		# Uniform-or-nothing (spec): a mix with field children is not a block array.
		if self.arena[parent].children:
			self._err(line, "list element mixed with field children; ignored")
			self.lost += 1
			return
		trimmed = _trim(body)
		if not trimmed:
			self._err(line, "empty list element")
			self.lost += 1
			return
		# One scalar per line; a bare comma is an error, not a second element.
		if len(_split_unquoted_commas(trimmed)) > 1:
			self._err(line, "bare comma in list element (one element per line)")
			self.lost += 1
			return
		if _unterminated_quote(trimmed):
			self._err(line, "unterminated quote in value")
		el = _parse_element(trimmed)
		if el is None:
			self._err(line, "empty list element")
			self.lost += 1
			return
		node = self.arena[parent]
		if node.value.kind == "empty":
			old_key = _merge_key(node.name, node.value)
			old_disp = (node.name, _disp_key(node.value))
			node.value = _cell([el])
			node.star_list = True
			# First element: remap now (Empty -> cell changes both keys), then
			# open the deferral window with the current keys. Rebuilding the
			# keys per appended element was O(list^2) time; the maps only need
			# to be fresh when queried, and every query flushes first.
			self._remap_child(parent, old_key, old_disp)
			k = _merge_key(node.name, node.value)
			d = (node.name, _disp_key(node.value))
			self.star_open = (parent, k, d)
		elif node.value.kind == "cell" and node.star_list:
			if self.star_open is None or self.star_open[0] != parent:
				self._star_flush()
				old_key = _merge_key(node.name, node.value)
				old_disp = (node.name, _disp_key(node.value))
				self.star_open = (parent, old_key, old_disp)
			node.value.els.append(el)
		else:
			self._err(line, "field already has a value; list element ignored")
			self.lost += 1

	def _emit_repeated_leaf_hints(self):
		"""Legal input that looks like a common mistake: a field repeating as a bare
		scalar leaf. Mandatory hint per spec (never fails a load)."""
		hints = []
		for parent in range(len(self.arena)):
			# Group by name in first-appearance order: hint order must be
			# deterministic or the cross-binding check can't compare `check` output.
			by_name: list = []
			group_of: dict = {}
			for c in self.arena[parent].children:
				name = self.arena[c].name
				g = group_of.get(name)
				if g is not None:
					by_name[g][1].append(c)
				else:
					group_of[name] = len(by_name)
					by_name.append((name, [c]))
			for name, group in by_name:
				if len(group) < 2:
					continue
				all_scalar_leaves = all(
					not self.arena[c].children
					and self.arena[c].value.kind == "cell"
					and not self.arena[c].star_list
					for c in group
				)
				if all_scalar_leaves:
					line = max(self.arena[c].line for c in group)
					joined = ", ".join(self.arena[c].value.display() for c in group)
					hints.append((line, f"{_h001_head(name)}{joined}'?"))
		for line, message in hints:
			self.diags.append(Diagnostic(line, Severity.Hint, message, "H001"))

	def parse(self, text, strictness):
		# UTF-8 BOM strip, then split keeping raw lines (CR stripped per line).
		if text.startswith("﻿"):
			text = text[1:]
		# The whole trailing CR run goes, not just one: a raw block keeps its
		# content untrimmed, so a line left ending in CR would be written back as
		# CRLF and read as neither - the one shape where the count is visible.
		lines = [ln.rstrip("\r") for ln in text.split("\n")]
		i = 0
		nlines = len(lines)
		while i < nlines:
			lineno = i + 1
			line = _trim_end(lines[i])
			j = 0
			while j < len(line) and (line[j] == " " or line[j] == "\t"):
				j += 1
			indent = line[:j]
			rest = line[j:]
			if not rest:
				self.saw_blank = True
				i += 1
				continue
			# Whole-line comment: hold it for the next line that binds a node.
			# It consumes a pending blank into its own flag, so a blank between
			# comment-only regions survives the round-trip.
			if rest.startswith("#"):
				self.pending.append(_Pend(rest, indent, self.saw_blank))
				self.saw_blank = False
				i += 1
				continue
			# Any other line consumes the pending blank; only a field line that
			# binds turns it into grouping.
			had_blank = self.saw_blank
			self.saw_blank = False
			# A binding line claims the pending comments - but deeper-written
			# ones hang on their own block first.
			self._hang_deeper_pending(indent)
			# Child-indent fence: a value line for its parent field.
			fo = _fence_open(rest)
			if fo is not None:
				ch, length, info = fo
				parent = self._resolve_parent(indent)
				if parent is None:
					self._err(lineno, "indentation matches no open level")
					self.lost += 1
					i += 1
					continue
				value, nxt = self._consume_raw(lines, i + 1, lineno, ch, length, info)
				node = self._bind_block(parent, value, lineno)
				if node is not None:
					self._attach_trivia(node, "")
				i = nxt
				continue
			# Stacked-list element: colon-less by construction ('*' can't begin a name).
			if rest.startswith("*"):
				after = rest[1:]
				if after.startswith(" ") or after.startswith("\t"):
					parent = self._resolve_parent(indent)
					if parent is None:
						self._err(lineno, "indentation matches no open level")
						self.lost += 1
						i += 1
						continue
					body, comment = _split_comment(after)
					# Elements have no node of their own; trivia rides the field.
					if parent != ROOT:
						self._attach_trivia(parent, comment)
					self._add_star_element(parent, body, lineno)
					i += 1
					continue
				self._err(lineno, "malformed line: '*' must be followed by a space")
				# Content-malformed at any position, so it is safe to retain
				# verbatim as trivia: re-emitted, it re-diagnoses identically
				# and can never read as a live binding. A hand-typo no longer
				# vanishes on the consumer's next save. The BOM exception the
				# sibling site below carries cannot apply here: this line
				# starts with the '*' that brought us in.
				self.pending.append(_Pend(_trim_end(rest), indent, had_blank))
				i += 1
				continue
			# Field line.
			before, comment = _split_comment(rest)
			content = _trim_end(before)
			if not content:
				# Only a comment survived (e.g. an escaped lead-in); keep it.
				if comment:
					self.pending.append(_Pend(comment, indent, had_blank))
				i += 1
				continue
			parent = self._resolve_parent(indent)
			if parent is None:
				self._err(lineno, "indentation matches no open level")
				self.lost += 1
				i += 1
				continue
			try:
				segments, value_text = _scan_path(content)
			except _PathError as e:
				self._err(lineno, f"malformed line skipped: {e.args[0]}")
				# Content-malformed at any position - retained as trivia, same
				# rationale (and same BOM exception) as the bad '*' line above.
				if rest.startswith("\ufeff"):
					self.lost += 1
				else:
					self.pending.append(_Pend(_trim_end(rest), indent, had_blank))
				i += 1
				continue
			nxt = i + 1
			# The verbatim value span, kept for reads' `raw` (only the plain
			# scalar/inline-array case has a one-line source spelling).
			src_text = None
			if value_text is None:
				# A clean path with no colon is the one defined repair:
				# the obvious intent is that path with an empty value.
				self._err(lineno, "missing colon; repaired as an empty value")
				value = _empty()
			elif value_text == "":
				value = _empty()
			else:
				fo = _fence_open(value_text)
				if fo is not None:
					# Same-line fence spelling.
					ch, length, info = fo
					value, nxt = self._consume_raw(lines, i + 1, lineno, ch, length, info)
				else:
					if _unterminated_quote(value_text):
						self._err(lineno, "unterminated quote in value")
					src_text = value_text
					value = _parse_cell(value_text)
			# Record only when the bound node holds exactly this line's value
			# (a merge into an equal-valued node keeps the first line's span;
			# a value dropped after a last-segment selector records nothing).
			vkey = _value_key(value) if src_text is not None else None
			node = self._attach_path(parent, segments, value, lineno)
			if node is not None:
				if src_text is not None and not self.arena[node].src_set and _value_key(self.arena[node].value) == vkey:
					self.arena[node].src_set = True
					if not _src_matches_display(self.arena[node].value, src_text):
						self.arena[node].src = src_text
				if had_blank:
					self.arena[node].blank_before = True
				self._attach_trivia(node, comment)
				self.stack.append((indent, node))
			i = nxt
		self._star_flush()
		self._fold_late_dups()
		self._emit_repeated_leaf_hints()
		# Indented tail comments keep their block; only top-level ones orphan.
		self._hang_deeper_pending("")
		orphans = [_Lead(p.text, p.blank_before) for p in self.pending]
		self.pending = []
		return Document(self.arena, self.diags, strictness, orphans, self.lost)


# ---------------------------------------------------------------------------
# Document: load, diagnostics, formatter
# ---------------------------------------------------------------------------

_NO_DEFAULT = object()  # get_* sentinel: no call-site default -> must-exist (raises)


class Document:
	"""A parsed SHCL document: the tree, its diagnostics, and its strictness level."""
	__slots__ = ("arena", "diags", "_strictness", "orphans", "_lost")

	def __init__(self, arena, diags, strictness, orphans=None, lost=0):
		self.arena = arena
		self.diags = diags
		self._strictness = strictness
		self.orphans = orphans if orphans is not None else []
		# Lines or values parsing dropped that canonical output cannot re-emit
		# (bad indentation, an unusable selector, past the depth cap, ...).
		# Content-malformed lines are NOT counted - they are retained as trivia
		# and survive a save. lost_count() serves it; save_file() gates on it.
		self._lost = lost

	@staticmethod
	def parse(text):
		"""Parse at Standard strictness. Never fails: bad lines are skipped and
		diagnosed, good values stay readable."""
		return _Parser().parse(text, Strictness.Standard)

	@staticmethod
	def parse_with(text, strictness):
		"""Parse at a chosen strictness. Only Strict can fail (any error
		diagnostic); the raised LoadError still carries the parsed document
		alongside the diagnostics."""
		doc = _Parser().parse(text, strictness)
		if strictness == Strictness.Strict and any(d.severity == Severity.Error for d in doc.diags):
			raise LoadError(doc.diags, doc)
		return doc

	@staticmethod
	def load_file(path):
		"""File tier, load half: read and parse path at Standard. Never fails -
		the document always comes back usable (empty when the file could not be
		read), and the returned (document, FileStatus) pair separates the four
		cases consumers otherwise confuse: absent, present-but-unreadable,
		parsed with errors, clean."""
		return Document.load_file_with(path, Strictness.Standard)

	@staticmethod
	def load_file_with(path, strictness):
		"""load_file at a chosen strictness. A strict-failing file reports
		HadErrors; the recover-and-continue document still comes back."""
		try:
			with open(path, encoding="utf-8", newline="") as f:
				text = f.read()
		except FileNotFoundError:
			return _Parser().parse("", strictness), FileStatus.NotFound
		except (OSError, UnicodeDecodeError, ValueError):
			# ValueError is not decorative: a NUL in the path raises it rather
			# than an OSError, and this call promises a status, never a throw.
			return _Parser().parse("", strictness), FileStatus.Unreadable
		doc = _Parser().parse(text, strictness)
		if any(d.severity == Severity.Error for d in doc.diags):
			return doc, FileStatus.HadErrors
		return doc, FileStatus.Clean

	def save_file(self, path):
		"""File tier, save half: write this document's canonical text to path
		through a temp file in the same directory plus a rename, so an
		interrupted save can never truncate the config it rewrites - the same
		mechanics the CLI's --write uses. Refuses when parsing lost content that
		a save would silently delete (see lost_count); save_file_lossy writes
		anyway. Raises SaveRefused for the gate and SaveFailed for a failed
		write: returning the message instead would let the obvious spelling -
		the call on a line of its own - report success while doing nothing."""
		if self._lost > 0:
			raise SaveRefused(path, self._lost)
		err = write_file_atomic(path, self.to_canonical())
		if err is not None:
			raise SaveFailed(err)

	def save_file_lossy(self, path):
		"""save_file without the lost-content gate: writes even when parsing
		dropped lines this save deletes. The caller owns that choice. Never
		raises SaveRefused - the gate is the one thing it skips."""
		err = write_file_atomic(path, self.to_canonical())
		if err is not None:
			raise SaveFailed(err)

	def diagnostics(self):
		return self.diags

	def lost_count(self):
		"""How many lines or values parsing dropped that canonical output cannot
		re-emit - bad indentation, an unusable selector, a line past the depth
		cap. Content-malformed lines do NOT count: those are retained as trivia
		and survive a save. Nonzero means a save_file would delete hand-written
		content, so save_file refuses then (save_file_lossy overrides)."""
		return self._lost

	def error_count(self):
		"""How many error-severity diagnostics the document carries - the "did
		this file have errors?" predicate, so recover-and-continue can't read
		as success by accident. Counts whatever diagnostics() holds (after
		load_and_validate, that includes validation errors)."""
		return sum(1 for d in self.diags if d.severity == Severity.Error)

	@staticmethod
	def load_and_validate(text, schema_text, strictness):
		"""One-shot load-and-validate: parse at a strictness, validate against a
		schema, and hand back the document carrying ONE combined diagnostics
		list (parse first, then validation - the order `check --schema`
		prints), so half the errors can't vanish because a caller forgot one
		of the two lists. Never fails: a strict-failing document comes back as
		the document plus its diagnostics (error_count() answers "did it
		fail"). An empty schema text skips validation entirely. H001 hints the
		schema disavows (a declared repeat upper bound above 1) are dropped."""
		doc = _Parser().parse(text, strictness)
		if _trim(schema_text):
			schema = Document.parse(schema_text)
			# A schema that did not load would silently drop the constraints on
			# its broken lines, or report every field as unknown - either way
			# blaming the document for the schema. Say so instead, as `check`
			# does, and validate nothing.
			if any(d.severity == Severity.Error for d in schema.diags):
				doc.diags.append(Diagnostic(0, Severity.Error, "schema failed to load", "V099"))
				return doc
			vdiags = doc.validate(schema)
			doc.diags.extend(vdiags)
			suppress_declared_repeats(schema, doc.diags)
			suppress_declared_reopens(schema, doc.diags)
		return doc

	def strictness(self):
		return self._strictness

	# Formatter

	def to_canonical(self):
		"""Canonical form: block layout, tabs, insertion order, minimal quoting,
		redundancy collapsed, comments re-emitted as attached trivia. Scalar
		text is never rewritten."""
		# Explicit stack, children pushed in reverse: the reference handles depths
		# far past Python's recursion limit, so emit must not recurse.
		out: list = []
		stack: list = []
		self._emit_children(self.arena[ROOT].children, 0, stack)
		while stack:
			idx, depth, would_merge = stack.pop()
			if would_merge is None:
				# Post-children marker. Comments this block owns with no child
				# to carry them re-emit one deeper.
				ipad = "\t" * (depth + 1)
				for c in self.arena[idx].inside():
					if c.blank_before and out:
						out.append("\n")
					out.append(ipad)
					out.append(c.text)
					out.append("\n")
				# Comments that hung on this block after its last child re-emit
				# at the block's own depth.
				pad = "\t" * depth
				for c in self.arena[idx].after():
					if c.blank_before and out:
						out.append("\n")
					out.append(pad)
					out.append(c.text)
					out.append("\n")
				continue
			self._emit_node(idx, depth, would_merge, out)
			if self.arena[idx].after() or self.arena[idx].inside():
				# The marker sits under the children, so it pops after them.
				stack.append((idx, depth, None))
			self._emit_children(self.arena[idx].children, depth + 1, stack)
		# Comments that never found a following line re-emit at the end.
		for c in self.orphans:
			if c.blank_before and out:
				out.append("\n")
			out.append(c.text)
			out.append("\n")
		return "".join(out)

	def _emit_children(self, kids, depth, stack):
		"""Emit a sibling run. The parent walk already knows whether an earlier
		same-name sibling is empty (the raw same-line-fence hazard), so one
		seen-empties set here replaces a per-child rescan of the whole run.
		Iterative shape: the flags are computed per run and ride the stack."""
		entries = []
		empties = set()
		for c in kids:
			n = self.arena[c]
			wm = n.value.kind == "raw" and n.name in empties
			if n.value.is_empty():
				empties.add(n.name)
			entries.append((c, depth, wm))
		stack.extend(reversed(entries))

	def _emit_node(self, idx, depth, would_merge, out):
		node = self.arena[idx]
		pad = "\t" * depth
		v = node.value
		# Same-line fence spelling can't carry an inline comment (an unbalanced
		# quote in the info-string could hide the `#` on reparse), so its
		# trailing comment joins the leading lines instead; the flag comes from
		# the parent's walk. Each blank rides its own comment (or the binding
		# line), never as the first output line.
		trailing = node.trailing()
		for c in node.leading():
			if c.blank_before and out:
				out.append("\n")
			out.append(pad)
			out.append(c.text)
			out.append("\n")
		if node.blank_before and out:
			out.append("\n")
		if would_merge and trailing:
			out.append(pad)
			out.append(trailing)
			out.append("\n")
		out.append(pad)
		out.append(_emit_name(node.name))
		out.append(":")
		if v.kind == "empty":
			if trailing:
				out.append("  ")
				out.append(trailing)
			out.append("\n")
		elif v.kind == "cell":
			out.append(" ")
			out.append(", ".join(_emit_element(e) for e in v.els))
			if trailing:
				out.append("  ")
				out.append(trailing)
			out.append("\n")
		else:
			# Child-indent spelling is canonical: bare name line, fenced block one
			# level deeper, verbatim content. Exception: if an earlier same-name
			# sibling is empty, the bare `name:` header would merge into it on
			# reparse and the fence would fill that instance instead - so use the
			# same-line spelling there.
			if would_merge:
				out.append(" ")
			else:
				if trailing:
					out.append("  ")
					out.append(trailing)
				out.append("\n")
			body_pad = "\t" * (depth + 1)
			fence = v.fence_char * v.fence_len
			if not would_merge:
				out.append(body_pad)
			out.append(fence)
			if v.info:
				# An info-string starting with the fence char would extend the run
				# on reparse; a space keeps the fence length intact.
				if v.info[0] == v.fence_char:
					out.append(" ")
				out.append(v.info)
			out.append("\n")
			if v.content:
				# A body with no non-blank line has no common indent for the
				# reload to strip back off, so indenting it here would add a
				# level on every pass, without bound. Leave it as it stands.
				all_blank = all(not _trim(ln) for ln in v.content.split("\n"))
				for ln in v.content.split("\n"):
					if ln and not all_blank:
						out.append(body_pad)
					out.append(ln)
					out.append("\n")
			out.append(body_pad)
			out.append(fence)
			out.append("\n")

	# Accessor: path resolution

	def _children_named(self, parent, name):
		return [c for c in self.arena[parent].children if self.arena[c].name == name]

	def _resolve_from(self, start, segs):
		# Returns ("none",) | ("one", idx) | ("many", [idx]) | ("slots", [entry]).
		# A slots entry is a node idx, or the Status saying why the sub-path did
		# not land on one node (NotFound missing, Multiple ambiguous).
		cur = list(start)
		for i, seg in enumerate(segs):
			nxt = []
			for node in cur:
				if seg.star:
					nxt.extend(self.arena[node].children)
				else:
					nxt.extend(self._children_named(node, seg.name))
			if seg.star:
				# Name wildcard: same per-slot split as `[*]`, over every child.
				rest = segs[i + 1:]
				slots = []
				for inst in nxt:
					if not rest:
						slots.append(inst)
					else:
						r = self._resolve_from([inst], rest)
						if r[0] == "one":
							slots.append(r[1])
						elif r[0] == "none":
							slots.append(Status.NotFound)
						else:
							slots.append(Status.Multiple)
				return ("slots", slots)
			sel = seg.selector
			if sel is None:
				cur = nxt
			elif sel[0] == "val":
				want = _apply_escapes(sel[1])
				cur = [c for c in nxt if _disp_key(self.arena[c].value) == want and (not sel[2] or _single_scalar(self.arena[c].value))]
			elif sel[0] == "idx":
				k = sel[1]
				cur = [nxt[k]] if k < len(nxt) else []
			else:
				# Wildcard: remaining path resolves per-instance; slots stay aligned.
				rest = segs[i + 1:]
				slots = []
				for inst in nxt:
					if not rest:
						slots.append(inst)
					else:
						r = self._resolve_from([inst], rest)
						if r[0] == "one":
							slots.append(r[1])
						elif r[0] == "none":
							slots.append(Status.NotFound)
						else:
							slots.append(Status.Multiple)
				return ("slots", slots)
		if len(cur) == 0:
			return ("none",)
		if len(cur) == 1:
			return ("one", cur[0])
		return ("many", cur)

	def _resolve(self, path):
		# Returns a _resolve_from result, or ("err", Status).
		try:
			segments, value_text = _scan_lookup(path)
		except _PathError:
			return ("err", Status.NotFound)
		if value_text is not None:
			return ("err", Status.NotFound)   # a query has no value part
		return self._resolve_from([ROOT], segments)

	def count(self, path):
		"""Instance count at a path (0 when nothing matches)."""
		r = self._resolve(path)
		tag = r[0]
		if tag == "one":
			return 1
		if tag == "many" or tag == "slots":
			return len(r[1])
		return 0

	def paths(self):
		"""Every field path in the document, in file order, deduplicated - a
		query recipe for tooling. A segment that is not bare-name-safe is
		emitted quoted and escaped - the form the path scanner accepts - so
		each path is a well-formed lookup path and nothing in the document is
		hidden."""
		out = []
		seen = set()
		stack = [(c, "") for c in reversed(self.arena[ROOT].children)]
		while stack:
			node, prefix = stack.pop()
			seg = _emit_name(self.arena[node].name)
			path = seg if not prefix else prefix + "." + seg
			if path not in seen:
				seen.add(path)
				out.append(path)
			for c in reversed(self.arena[node].children):
				stack.append((c, path))
		return out

	def line(self, path):
		"""1-based source line of the binding at a path, for consumer checks the
		schema cannot express. 0 when the path does not resolve to exactly one
		node, or the node was writer-built. Merged instances cite the first
		binding's line, matching diagnostics."""
		r = self._resolve(path)
		if r[0] == "one":
			return self.arena[r[1]].line
		return 0

	def authored_name(self, path):
		"""The field name at a path exactly as the author spelled it (case
		unfolded, outer quotes stripped), so a message can echo `SYMBOLS` when
		the file said SYMBOLS. Escape sequences stay as written too: a name is
		stored, compared and emitted with its escapes RESOLVED, so this is the
		one call that hands the source spelling back - which is what an
		as-authored accessor is for. Resolution mirrors line(): empty when the path
		does not resolve to exactly one node. Merged instances keep the first
		binding's spelling; a writer-built node keeps the spelling the setter's
		path used."""
		r = self._resolve(path)
		if r[0] == "one":
			return self.arena[r[1]].name_src
		return ""

	def lines(self, path):
		"""The plural line(): 1-based source lines at a path, in file order, so
		a repeated field - the case that most wants a citable line - yields
		every binding's. Wildcard slots that did not resolve stay in the list
		as 0, and a writer-built node is 0, so indices keep matching count()."""
		r = self._resolve(path)
		tag = r[0]
		if tag == "one":
			return [self.arena[r[1]].line]
		if tag == "many":
			return [self.arena[n].line for n in r[1]]
		if tag == "slots":
			return [self.arena[n].line if isinstance(n, int) else 0
				for n in r[1]]
		return []

	def children(self, path):
		"""Child field names under a path, in file order, duplicates included -
		the "what keys are in this section?" question paths() (deduplicated,
		path-shaped) cannot answer. "" enumerates the top level. Names come
		back as stored; quote_segment() makes one splice-safe in a path."""
		if not _trim(path):
			node = ROOT
		else:
			r = self._resolve(path)
			if r[0] != "one":
				return []
			node = r[1]
		return [self.arena[c].name for c in self.arena[node].children]

	def instances(self, path):
		"""Instance values at a path, in file order. Wildcard slots that did not
		resolve stay in the list as "" so indices keep matching count()."""
		r = self._resolve(path)
		tag = r[0]
		if tag == "one":
			return [self.arena[r[1]].value.display()]
		if tag == "many":
			return [self.arena[n].value.display() for n in r[1]]
		if tag == "slots":
			return [self.arena[n].value.display() if isinstance(n, int) else ""
				for n in r[1]]
		return []

	# Writer: typed emit, defaults, comments, structural edits
	# The reverse of the Accessor. A setter builds the canonical stored text for a
	# typed value (the inverse of the matching read) and places it at a path,
	# creating intermediate nodes on the way. Reads and to_canonical walk the
	# children lists, so mutating the arena directly is enough.

	@staticmethod
	def new():
		"""A fresh document with no bindings - the start point for schema-driven
		generation. Set values, then to_canonical()."""
		return Document.parse("")

	def _new_child(self, parent, name, name_src, value):
		idx = len(self.arena)
		node = _Node(name, value, parent, 0, name_src)
		# Hand-written files separate top-level sections with a blank line;
		# writer-built ones do the same (the emitter never blanks line 1).
		node.blank_before = parent == ROOT
		self.arena.append(node)
		self.arena[parent].children.append(idx)
		return idx

	def write_reason(self, path):
		"""Why a write at this path would fail - the reason behind a setter's
		bare False, so a consumer's error message need not guess. Writable means
		the same validation _place() runs would pass; nothing is created."""
		try:
			segments, value_text = _scan_lookup(path)
		except _PathError:
			return WriteReason.BadPath
		return self._probe_write(segments, value_text)[0]

	def _probe_write(self, segments, value_text, trail=None):
		"""The validation walk write_reason and _place share. `trail`, when a
		list is passed, collects where each segment landed - None from the point
		the path falls off the existing tree - so _place can create from exactly
		there instead of scanning the path and walking the tree a second time."""
		if value_text is not None:
			return (WriteReason.ValueInPath, None)
		if not segments:
			return (WriteReason.BadPath, None)
		# Writer side of the load-time nesting cap: never create deeper.
		if len(segments) > MAX_DEPTH:
			return (WriteReason.TooDeep, None)
		# The probe walk _place() validates with: once it falls off the existing
		# tree, a later `[#k]` can never match (fresh intermediates are created
		# childless), so an index segment past that point is unresolvable.
		probe = ROOT
		for seg in segments:
			if seg.star:
				return (WriteReason.Wildcard, None)
			sel = seg.selector
			# A newline in a SELECTOR has no one-line spelling, so the emitted
			# binding would split across two lines and reparse as neither. The
			# selector stores its path text raw and the value emitter never
			# escapes a line break, so nothing downstream can rescue it - and the
			# reload loses nothing it can count, so the save gate would not catch
			# it. A newline in a NAME is fine: names are stored escape-resolved
			# and emitted through the name escaper, which spells a line break \n
			# and reads it back as one.
			if sel is not None and sel[0] == "val" and "\n" in sel[1]:
				return (WriteReason.BadPath, None)
			if sel is not None and sel[0] == "wild":
				return (WriteReason.Wildcard, None)
			if sel is not None and sel[0] == "idx":
				if probe is None:
					return (WriteReason.NoSuchIndex, None)
				matches = [c for c in self.arena[probe].children if self.arena[c].name == seg.name]
				if sel[1] >= len(matches):
					return (WriteReason.NoSuchIndex, None)
				probe = matches[sel[1]]
			elif sel is not None and sel[0] == "val":
				if probe is not None:
					want = _apply_escapes(sel[1])
					found = None
					for c in self.arena[probe].children:
						if self.arena[c].name == seg.name and _disp_key(self.arena[c].value) == want and (not sel[2] or _single_scalar(self.arena[c].value)):
							found = c
							break
					probe = found
			else:
				if probe is not None:
					found = None
					for c in self.arena[probe].children:
						if self.arena[c].name == seg.name:
							found = c
							break
					probe = found
			if trail is not None:
				trail.append(probe)
		return (WriteReason.Writable, trail)

	def _place(self, path):
		"""Walk (creating as needed) to the node a write targets. A trailing
		name with no selector hits the first same-named instance (or a new one);
		a `[value]` selector selects the matching instance or creates it; `[#k]`
		must already exist. None = path unusable for a write (write_reason()
		says why). Validation runs first, so a doomed path leaves no
		half-created intermediates behind."""
		try:
			segments, value_text = _scan_lookup(path)
		except _PathError:
			return None
		trail: list = []
		if self._probe_write(segments, value_text, trail)[0] != WriteReason.Writable:
			return None
		cur = ROOT
		for i, seg in enumerate(segments):
			# The probe already resolved every segment that exists; only the
			# tail it fell off has anything to create.
			if trail[i] is not None:
				cur = trail[i]
				continue
			sel = seg.selector
			if sel is None:
				cur = self._new_child(cur, seg.name, seg.name_src, _empty())
			elif sel[0] == "val":
				cur = self._new_child(cur, seg.name, seg.name_src, _cell_of(sel[1]))
			else:
				# Unreachable: _probe_write refuses a wildcard outright and an
				# unresolvable index, so neither reaches an empty trail slot.
				return None
		return cur

	def _set_value(self, path, value):
		idx = self._place(path)
		if idx is None:
			return False
		self.arena[idx].value = value
		self.arena[idx].src = None   # written value has no source spelling
		self._collapse_dup(idx)
		return True

	def _collapse_dup(self, node):
		# A written value may now collide with a same-named sibling under the
		# in-file merge rule; fold the pair the way a reparse would (earlier
		# sibling survives, later one folds children and trivia in) so Writer
		# output stays a formatter fixpoint.
		parent = self.arena[node].parent
		name = self.arena[node].name
		key = self.arena[node].value.key()
		other = None
		for c in self.arena[parent].children:
			if c != node and self.arena[c].name == name and self.arena[c].value.key() == key:
				other = c
				break
		if other is None:
			return
		siblings = self.arena[parent].children
		if siblings.index(other) < siblings.index(node):
			survivor, loser = other, node
		else:
			survivor, loser = node, other
		_fold_node_into(self.arena, survivor, loser)
		self.arena[parent].children = [c for c in self.arena[parent].children if c != loser]

	def exists(self, path):
		"""True when the path resolves to at least one real node."""
		r = self._resolve(path)
		tag = r[0]
		if tag == "one" or tag == "many":
			return True
		if tag == "slots":
			return any(isinstance(n, int) for n in r[1])
		return False

	def remove(self, path):
		"""Delete the node(s) at a path (with their subtrees); returns how many."""
		r = self._resolve(path)
		tag = r[0]
		if tag == "one":
			targets = [r[1]]
		elif tag == "many":
			targets = list(r[1])
		elif tag == "slots":
			targets = [n for n in r[1] if isinstance(n, int)]
		else:
			targets = []
		for t in targets:
			p = self.arena[t].parent
			self.arena[p].children = [c for c in self.arena[p].children if c != t]
		return len(targets)

	def set_comment(self, path, text):
		"""Attach a leading comment line to the node at a path (creating an empty
		node if absent). A missing '#' is added; only the first line is kept."""
		idx = self._place(path)
		if idx is None:
			return False
		line = text.split("\n", 1)[0]
		if not line.startswith("#"):
			line = "# " + line
		self.arena[idx]._triv().leading.append(_Lead(line, False))
		return True

	def set_int(self, path, v):
		"""Bind an integer at path, creating the path as needed; False = path not
		writable (write_reason says why - same for every setter). Worth checking
		rather than assuming: an ignored False means the save that follows writes
		a document missing the edit, and reports success doing it."""
		if not _fits_i64(v):
			return False
		return self._set_value(path, _cell_of(str(v)))

	def set_float(self, path, v):
		return self._set_value(path, _cell_of(format_float(v)))

	def set_bool(self, path, v):
		return self._set_value(path, _cell_of("true" if v else "false"))

	def set_string(self, path, v):
		return self._set_value(path, _cell_of(_encode_string(v)))

	def set_datetime(self, path, v):
		return self._set_value(path, _cell_of(str(v)))

	def set_raw(self, path, content, info):
		fc, fl = _choose_fence(content)
		return self._set_value(path, _raw(content, info, fc, fl))

	def set_empty(self, path):
		return self._set_value(path, _empty())

	def set_int_array(self, path, v):
		if not all(_fits_i64(x) for x in v):
			return False
		return self._set_value(path, _array_cell([str(x) for x in v]))

	def set_float_array(self, path, v):
		return self._set_value(path, _array_cell([format_float(x) for x in v]))

	def set_bool_array(self, path, v):
		return self._set_value(path, _array_cell(["true" if x else "false" for x in v]))

	def set_string_array(self, path, v):
		return self._set_value(path, _array_cell([_encode_string(x) for x in v]))

	def set_datetime_array(self, path, v):
		return self._set_value(path, _array_cell([str(x) for x in v]))

	# Default (only-if-absent) forms - the "emit defaults" half of the Writer.
	def set_int_default(self, path, v):
		if not self.exists(path):
			return self.set_int(path, v)
		return True

	def set_float_default(self, path, v):
		if not self.exists(path):
			return self.set_float(path, v)
		return True

	def set_bool_default(self, path, v):
		if not self.exists(path):
			return self.set_bool(path, v)
		return True

	def set_literal(self, path, text):
		"""Bind text at path as value syntax rather than as data.

		"80, 443" becomes a two-element array where set_string would store one
		string that has to be quoted. This is how a caller holding value text -
		a config line, a user's --set argument - writes it without knowing its
		shape first. Returns False on text that could not be one line's value.
		"""
		v = _literal_value(text)
		if v is None:
			return False
		return self._set_value(path, v)

	def set_literal_default(self, path, text):
		if not self.exists(path):
			return self.set_literal(path, text)
		return True

	def set_string_default(self, path, v):
		if not self.exists(path):
			return self.set_string(path, v)
		return True

	def set_datetime_default(self, path, v):
		if not self.exists(path):
			return self.set_datetime(path, v)
		return True

	def set_raw_default(self, path, content, info):
		if not self.exists(path):
			return self.set_raw(path, content, info)
		return True

	def set_int_array_default(self, path, v):
		if not self.exists(path):
			return self.set_int_array(path, v)
		return True

	def set_float_array_default(self, path, v):
		if not self.exists(path):
			return self.set_float_array(path, v)
		return True

	def set_bool_array_default(self, path, v):
		if not self.exists(path):
			return self.set_bool_array(path, v)
		return True

	def set_string_array_default(self, path, v):
		if not self.exists(path):
			return self.set_string_array(path, v)
		return True

	def set_datetime_array_default(self, path, v):
		if not self.exists(path):
			return self.set_datetime_array(path, v)
		return True

	# Layered loading: overlay a higher-priority document

	def merge(self, over):
		"""Overlay `over` (a higher-priority layer) onto self (the lower one).
		Container instances merge by (name, value) exactly like the in-file rule;
		a leaf name present in `over` replaces self's same-named children at that
		scope - provided those base children are leaves too - so scalars, arrays,
		and raw blocks get real override while a bare section header merges
		instead of wiping. over-only nodes are appended. Comment trivia rides
		with each node. Load(defaults, site, user) is a left fold of this: each
		later file overlaid on the earlier ones."""
		self._lost += over._lost
		self._overlay(ROOT, over, ROOT)
		# Layers commonly share a footer; keeping one copy of each keeps a
		# stack of files from repeating it once per layer.
		for o in over.orphans:
			if not any(e.text == o.text for e in self.orphans):
				self.orphans.append(_Lead(o.text, o.blank_before))

	# One grouping pass over each side, then a single children rebuild: the
	# old shape re-filtered the over side per distinct name and re-scanned
	# (and re-keyed) the base side per over node - three O(K^2) terms at one
	# parent, plus a full list rebuild per replaced name.
	def _adopt_trivia(self, base, over, ok):
		"""A matched instance keeps the base node, so the over side's comments have
		to move onto it or they are lost. Same rule as an in-file merge: leading
		concatenates in layer order, first trailing wins."""
		# Per-element copies: the merged document must not share list objects
		# with `over`, which the caller may still use.
		st = over.arena[ok].trivia
		if st is None:
			return
		bt = self.arena[base]._triv()
		bt.leading.extend(_Lead(c.text, c.blank_before) for c in st.leading)
		if st.trailing:
			if not bt.trailing:
				bt.trailing = st.trailing
			else:
				bt.leading.append(_Lead(st.trailing, False))
		bt.after.extend(_Lead(c.text, c.blank_before) for c in st.after)
		bt.inside.extend(_Lead(c.text, c.blank_before) for c in st.inside)

	def _overlay(self, base_parent, over, over_parent):
		"""Explicit stack rather than recursion, for the same reason _clone_subtree
		uses one. Each level's rebuild depends on nothing the deeper levels do, so
		deferring them changes no result; the walk stays depth-first and in order."""
		stack = [(base_parent, over_parent)]
		while stack:
			bp, op = stack.pop()
			stack.extend(reversed(self._overlay_level(bp, over, op)))

	def _overlay_level(self, base_parent, over, over_parent):
		"""One level of the overlay. Returns the (base, over) pairs whose subtrees
		still have to be merged, in order."""
		over_kids = list(over.arena[over_parent].children)
		# Over side: name -> node bucket, in first-appearance order.
		order = []
		groups: dict = {}
		for k in over_kids:
			n = over.arena[k].name
			g = groups.get(n)
			if g is None:
				order.append(n)
				g = []
				groups[n] = g
			g.append(k)
		# Base side, one pass: does the name have a container instance, and
		# which child carries each (name, key) - every key computed once.
		base_kids = list(self.arena[base_parent].children)
		has_container: dict = {}
		by_key: dict = {}
		for b in base_kids:
			name = self.arena[b].name
			has_container[name] = has_container.get(name, False) or bool(self.arena[b].children)
			by_key.setdefault((name, self.arena[b].value.key()), b)
		# Decide per name. A name whose over-side nodes are all leaves is an
		# override - but only when the base side of the group is leaf-shaped
		# too. Against a base container, a childless over-node is a wrapper
		# mention, not a leaf, so it falls through to the instance merge: a
		# bare section header in a higher layer never wipes the subtree below.
		# Replaced groups splice in the rebuild; everything appended (unmatched
		# instances, and replaced names base never had) keeps processing order.
		replace = {}
		appended = []
		pending = []
		empty_key = _Value("empty").key()
		for name in order:
			group = groups[name]
			over_leafy = all(not over.arena[k].children for k in group)
			in_base = name in has_container
			base_container = has_container.get(name, False)
			if over_leafy and not base_container:
				clones = [self._clone_subtree(over, ok, base_parent) for ok in group]
				if in_base:
					replace[name] = clones
				else:
					appended.extend(clones)
			else:
				for ok in group:
					okey = over.arena[ok].value.key()
					b = by_key.get((name, okey))
					# A raw block in the higher layer fills a same-named empty
					# binding below, exactly as a fence line fills one inside a
					# single file. Without it, merging two documents and parsing
					# them run together disagree: both bindings survive here and
					# fold there, so merged output is not a formatter fixpoint.
					if b is None and over.arena[ok].value.kind == "raw":
						hit = by_key.get((name, empty_key))
						if hit is not None:
							self.arena[hit].value = over.arena[ok].value.copy()
							del by_key[(name, empty_key)]
							by_key.setdefault((name, okey), hit)
							b = hit
					if b is not None:
						self._adopt_trivia(b, over, ok)
						# A name that reaches here is never in `replace`, so `b`
						# survives the rebuild below and can wait for it.
						pending.append((b, ok))
					else:
						appended.append(self._clone_subtree(over, ok, base_parent))
		if not replace and not appended:
			return pending
		# Rebuild once: each replaced group lands at its name's first original
		# position (dropped nodes stay in the arena, unreferenced - reads and
		# emit walk children from the root), appends go at the end.
		new_kids = []
		spliced = set()
		for b in base_kids:
			name = self.arena[b].name
			clones = replace.get(name)
			if clones is None:
				new_kids.append(b)
			elif name not in spliced:
				spliced.add(name)
				new_kids.extend(clones)
		new_kids.extend(appended)
		self.arena[base_parent].children = new_kids
		return pending

	def _clone_subtree(self, over, oi, parent):
		"""Deep-copy `over`'s subtree at `oi` into self's arena under `parent`.
		Explicit stack rather than recursion, for the same reason the parse and
		emit walks are iterative: python's frame budget is small enough that a
		document at the documented depth cap can exhaust it from an already-deep
		caller, and a raise part-way through leaves the base document mutated."""
		root = self._clone_node(over, oi, parent)
		stack = [(oi, root)]
		while stack:
			si, di = stack.pop()
			# Children are pushed reversed so they pop in source order; each
			# appends to its own parent, so sibling order is preserved.
			kids = []
			for ok in over.arena[si].children:
				ci = self._clone_node(over, ok, di)
				self.arena[di].children.append(ci)
				kids.append((ok, ci))
			stack.extend(reversed(kids))
		return root

	def _clone_node(self, over, oi, parent):
		"""One node of _clone_subtree: everything but the children."""
		src = over.arena[oi]
		# Copy the value too - sharing the object (and its element list) with
		# `over` would break the promise that the clone survives its release.
		cv = src.value.copy()
		node = _Node(src.name, cv, parent, src.line, src.name_src)
		node.star_list = src.star_list
		node.star_mixed = src.star_mixed
		st = src.trivia
		if st is not None:
			t = node._triv()
			t.leading = [_Lead(c.text, c.blank_before) for c in st.leading]
			t.trailing = st.trailing
			t.after = [_Lead(c.text, c.blank_before) for c in st.after]
			t.inside = [_Lead(c.text, c.blank_before) for c in st.inside]
		node.blank_before = src.blank_before
		node.src_set = src.src_set
		node.src = src.src
		idx = len(self.arena)
		self.arena.append(node)
		return idx

	# Accessor: typed reads

	def _node_at(self, path):
		# Returns ("ok", node index) or ("err", Status).
		r = self._resolve(path)
		tag = r[0]
		if tag == "err":
			return ("err", r[1])
		if tag == "none":
			return ("err", Status.NotFound)
		if tag == "many" or tag == "slots":
			return ("err", Status.Multiple)
		return ("ok", r[1])

	def _raw_of(self, n):
		"""A read's `raw`: the verbatim source value text when the value came from
		one source line, else the display form (writer-built, stacked list, raw
		block - shapes with no one-line source spelling)."""
		src = self.arena[n].src
		return src if src is not None else self.arena[n].value.display()

	def _scalar_element(self, v):
		# Returns ("ok", element) or ("err", Status).
		if v.kind == "empty":
			return ("err", Status.Empty)
		if v.kind == "raw":
			return ("err", Status.BadType)
		if len(v.els) == 1:
			return ("ok", v.els[0])
		return ("err", Status.BadType)   # an array is not one scalar

	def _read_scalar(self, path, coerce, default):
		na = self._node_at(path)
		if na[0] == "err":
			return Read(default, na[1], None)
		value = self.arena[na[1]].value
		raw = self._raw_of(na[1])
		line = self.arena[na[1]].line
		se = self._scalar_element(value)
		if se[0] == "err":
			return Read(default, se[1], raw)._at(line, False)
		v = coerce(se[1])
		if v is None:
			return Read(default, Status.BadType, raw)._at(line, se[1].quoted)
		return Read(v, Status.Good, raw)._at(line, se[1].quoted)

	def read_int(self, path):
		lvl = self._strictness
		return self._read_scalar(path, lambda e: _parse_int_text(e, lvl), 0)

	def read_float(self, path):
		lvl = self._strictness
		return self._read_scalar(path, lambda e: _parse_float_text(e, lvl), 0.0)

	def read_bool(self, path):
		lvl = self._strictness
		return self._read_scalar(path, lambda e: _parse_bool_text(e.text, lvl), False)

	def read_datetime(self, path):
		return self._read_scalar(path, lambda e: parse_datetime(e.text), ShclDateTime())

	def read_string(self, path):
		"""Any value reads as a string: a raw block yields its content, an array its
		canonical inline text. Escapes are applied."""
		na = self._node_at(path)
		if na[0] == "err":
			return Read("", na[1], None)
		value = self.arena[na[1]].value
		raw = self._raw_of(na[1])
		line = self.arena[na[1]].line
		if value.kind == "empty":
			return Read("", Status.Empty, raw)._at(line, False)
		if value.kind == "raw":
			return Read(value.content, Status.Good, raw)._at(line, False)
		if len(value.els) == 1:
			return Read(_apply_escapes(value.els[0].text), Status.Good, raw)._at(line, value.els[0].quoted)
		# Canonical inline form (quoting + escapes intact), so the string
		# re-parses to the same array - not the bare display join.
		return Read(", ".join(_emit_element(e) for e in value.els), Status.Good, raw)._at(line, False)

	def read_raw(self, path):
		"""Raw-block content (verbatim). Non-block values are BadType."""
		na = self._node_at(path)
		if na[0] == "err":
			return Read("", na[1], None)
		value = self.arena[na[1]].value
		raw = self._raw_of(na[1])
		line = self.arena[na[1]].line
		if value.kind == "raw":
			return Read(value.content, Status.Good, raw)._at(line, False)
		if value.kind == "empty":
			return Read("", Status.Empty, raw)._at(line, False)
		return Read("", Status.BadType, raw)._at(line, False)

	def read_raw_info(self, path):
		"""The advisory info-string of a raw block ("" when absent)."""
		na = self._node_at(path)
		if na[0] == "err":
			return Read("", na[1], None)
		raw = self._raw_of(na[1])
		line = self.arena[na[1]].line
		value = self.arena[na[1]].value
		if value.kind == "raw":
			return Read(value.info, Status.Good, raw)._at(line, False)
		return Read("", Status.BadType, raw)._at(line, False)

	def _read_array(self, path, coerce, default):
		r = self._resolve(path)
		tag = r[0]
		if tag == "err":
			return Read([], r[1], None)
		if tag == "slots":
			# Each slot reads like a scalar of the target type and records its
			# own status; the aggregate is the worst one (never silently Good).
			out = []
			sts = []
			for slot in r[1]:
				if not isinstance(slot, int):
					out.append(default)
					sts.append(slot)
					continue
				se = self._scalar_element(self.arena[slot].value)
				if se[0] == "err":
					out.append(default)
					sts.append(se[1])
					continue
				v = coerce(se[1])
				if v is None:
					out.append(default)
					sts.append(Status.BadType)
				else:
					out.append(v)
					sts.append(Status.Good)
			status = max(sts, key=lambda s: s.value) if sts else Status.Empty
			return Read(out, status, None, sts)
		if tag == "none":
			return Read([], Status.NotFound, None)
		if tag == "many":
			return Read([], Status.Multiple, None)
		# one
		value = self.arena[r[1]].value
		raw = self._raw_of(r[1])
		line = self.arena[r[1]].line
		if value.kind == "empty":
			return Read([], Status.Empty, raw)._at(line, False)
		if value.kind == "raw":
			return Read([], Status.BadType, raw)._at(line, False)
		out = []
		sts = []
		status = Status.Good
		for el in value.els:
			v = coerce(el)
			if v is None:
				out.append(default)
				sts.append(Status.BadType)
				status = Status.BadType
			else:
				out.append(v)
				sts.append(Status.Good)
		return Read(out, status, raw, sts)._at(line, False)

	def read_int_array(self, path):
		lvl = self._strictness
		return self._read_array(path, lambda e: _parse_int_text(e, lvl), 0)

	def read_float_array(self, path):
		lvl = self._strictness
		return self._read_array(path, lambda e: _parse_float_text(e, lvl), 0.0)

	def read_bool_array(self, path):
		lvl = self._strictness
		return self._read_array(path, lambda e: _parse_bool_text(e.text, lvl), False)

	def read_datetime_array(self, path):
		return self._read_array(path, lambda e: parse_datetime(e.text), ShclDateTime())

	def read_string_array(self, path):
		return self._read_array(path, lambda e: _apply_escapes(e.text), "")

	# Convenience/get tier: value on Good, else the call-site default. Pass a
	# `default` to make the read forgiving (Default mode - the call a beginner
	# writes 90% of the time; a missing/empty/bad/ambiguous read can't sneak in
	# as a real zero). With no default it must-exist, raising the Status. Array
	# forms fall back to the whole default list; per-slot substitution is the
	# read_*_array tier or the CLI --default.

	def _get(self, r, default):
		if r.status == Status.Good:
			return r.value
		if default is _NO_DEFAULT:
			raise StatusError(r.status)
		return default

	def get_int(self, path, default=_NO_DEFAULT):
		return self._get(self.read_int(path), default)

	def get_float(self, path, default=_NO_DEFAULT):
		return self._get(self.read_float(path), default)

	def get_bool(self, path, default=_NO_DEFAULT):
		return self._get(self.read_bool(path), default)

	def get_string(self, path, default=_NO_DEFAULT):
		return self._get(self.read_string(path), default)

	def get_raw(self, path, default=_NO_DEFAULT):
		return self._get(self.read_raw(path), default)

	def get_datetime(self, path, default=_NO_DEFAULT):
		return self._get(self.read_datetime(path), default)

	def get_int_array(self, path, default=_NO_DEFAULT):
		return self._get(self.read_int_array(path), default)

	def get_float_array(self, path, default=_NO_DEFAULT):
		return self._get(self.read_float_array(path), default)

	def get_bool_array(self, path, default=_NO_DEFAULT):
		return self._get(self.read_bool_array(path), default)

	def get_string_array(self, path, default=_NO_DEFAULT):
		return self._get(self.read_string_array(path), default)

	def get_datetime_array(self, path, default=_NO_DEFAULT):
		return self._get(self.read_datetime_array(path), default)

	# The same convenience reads with the fallback required rather than optional.
	# get_*(path, default) says the same thing and still works; these exist so
	# the spelling that means "with a fallback" is `_or` in every binding, and a
	# routine ported between two of them cannot keep the call name while changing
	# which tier it lands on.

	def get_int_or(self, path, default):
		return self._get(self.read_int(path), default)

	def get_float_or(self, path, default):
		return self._get(self.read_float(path), default)

	def get_bool_or(self, path, default):
		return self._get(self.read_bool(path), default)

	def get_string_or(self, path, default):
		return self._get(self.read_string(path), default)

	def get_raw_or(self, path, default):
		return self._get(self.read_raw(path), default)

	def get_datetime_or(self, path, default):
		return self._get(self.read_datetime(path), default)

	def get_int_array_or(self, path, default):
		return self._get(self.read_int_array(path), default)

	def get_float_array_or(self, path, default):
		return self._get(self.read_float_array(path), default)

	def get_bool_array_or(self, path, default):
		return self._get(self.read_bool_array(path), default)

	def get_string_array_or(self, path, default):
		return self._get(self.read_string_array(path), default)

	def get_datetime_array_or(self, path, default):
		return self._get(self.read_datetime_array(path), default)

	# --- Validator: schema-as-SHCL (see spec.md "Schema validation") ---------
	# The schema is an ordinary parsed document: a flat list of `field: <path>`
	# instances whose children are the constraints (closed vocabulary).
	# Validation reuses the accessor's path scan and the typed coercions, so
	# document strictness composes for free. Schema faults (V09x) come first
	# and the surviving constraints still check the document; the unknown-field
	# sweep skips only when a fault cost a path spelling. One line-number space
	# per result. The H001/H002 hints a schema disavows are NOT dropped here:
	# they live on the parse's diagnostics, which validation does not touch.
	# Parse then validate and they are still there - call
	# suppress_declared_repeats/suppress_declared_reopens yourself, or use
	# load_and_validate, which runs both for you.

	def validate(self, schema):
		"""Validate against a schema document (itself plain SHCL). Empty result
		= the document conforms. Diagnostic lines are document lines (0 =
		document scope); schema faults (V09x, schema-file lines) come first,
		and the surviving constraints still check the document. The
		unknown-field sweep runs too, unless a fault cost the schema a path
		spelling (an unreadable `field:` path, or a mount naming no declared
		fragment) - only those can turn declared fields into false unknowns;
		a key-level fault keeps its entry's chain."""
		sdef, faults = _build_schema(schema)
		out = faults
		for c in sdef.cons:
			self._v_check(c, sdef, out)
		if sdef.paths_complete:
			self._v_unknown(sdef, out)
		return out

	def _v_contexts(self, start, segs, anchor, out):
		# Resolution contexts: the whole document for a plain path; each
		# enclosing instance for the part of a path after a wildcard. required/
		# repeat evaluate per context (anchor line 0 = document scope), so
		# `server[*].port` + required means a port under EACH server -
		# vacuously true with no servers.
		cur = list(start)
		for i, seg in enumerate(segs):
			nxt = []
			for n in cur:
				if seg.star:
					nxt.extend(self.arena[n].children)
				else:
					nxt.extend(self._children_named(n, seg.name))
			if seg.star:
				# Name wildcard: same per-instance split as `[*]`, any child name.
				rest = segs[i + 1:]
				if not rest:
					out.append((anchor, nxt))
				else:
					for inst in nxt:
						self._v_contexts([inst], rest, self.arena[inst].line, out)
				return
			sel = seg.selector
			if sel is None:
				cur = nxt
			elif sel[0] == "val":
				want = _apply_escapes(sel[1])
				cur = [c for c in nxt if _disp_key(self.arena[c].value) == want and (not sel[2] or _single_scalar(self.arena[c].value))]
			elif sel[0] == "idx":
				cur = [nxt[sel[1]]] if sel[1] < len(nxt) else []
			else:
				rest = segs[i + 1:]
				if not rest:
					out.append((anchor, nxt))
				else:
					for inst in nxt:
						self._v_contexts([inst], rest, self.arena[inst].line, out)
				return
		out.append((anchor, cur))

	def _v_check(self, c, sdef, out):
		self._v_check_from(c, sdef, ROOT, 0, out, set())

	# A mounted fragment's fields run per resolved node, right after that
	# node's own checks, in fragment order - depth-first, so diagnostic order
	# stays derivable. Termination is structural: every mount descends at
	# least one document level, and the document is finite.
	def _v_check_from(self, c, sdef, start, anchor0, out, mounted):
		ctxs: list = []
		self._v_contexts([start], c.segs, anchor0, ctxs)
		for anchor, found in ctxs:
			if c.required and not found:
				_vdiag(out, anchor, f"required path missing: {c.path}")
			if c.repeat is not None:
				lo, hi = c.repeat
				n = len(found)
				if n < lo or n > hi:
					_vdiag(out, anchor, f"instance count out of bounds at '{c.path}': {n} not in {lo}..{hi}")
			for n in found:
				self._v_node(c, n, out)
				if c.inherits is not None:
					fcs = sdef.frags.get(c.inherits)
					if fcs is not None:
						# Two constraints can resolve to the same node and mount the
						# same fragment there. The second mount would repeat the
						# first's work and its diagnostics, and repeating it per
						# level is what makes a recursive schema cost double per
						# document level, so each pair is done once.
						if (c.inherits, n) not in mounted:
							mounted.add((c.inherits, n))
							for fc in fcs:
								self._v_check_from(fc, sdef, n, self.arena[n].line, out, mounted)

	def _v_node(self, c, n, out):
		node = self.arena[n]
		line = node.line
		base = c.ty[:-6] if c.ty is not None and c.ty.endswith("-array") else c.ty
		is_array = c.ty is not None and c.ty.endswith("-array")

		def wrong():
			_vdiag(out, line, f"wrong type at '{c.path}': value is not a valid {c.ty}")

		if node.value.kind == "empty":
			# Empty passes everything; required already counted it as present.
			return
		if node.value.kind == "raw":
			# A raw block satisfies `raw` and scalar `string` (any value reads
			# as a string); every other kind is a type miss.
			if c.ty is not None and ((base != "raw" and base != "string") or is_array):
				wrong()
				return
			if c.allowed is not None and c.allowed[0] == "strings":
				if node.value.content not in c.allowed[1]:
					_vdiag(out, line, f"value not allowed at '{c.path}': {node.value.content}")
			return
		els = node.value.els
		if base == "raw":
			wrong()
			return
		# A scalar kind on a multi-element value is the array-where-one-scalar-
		# expected miss - except string, which reads arrays.
		if c.ty is not None and not is_array and base != "string" and len(els) > 1:
			wrong()
			return
		if base == "int":
			vals = []
			for e in els:
				v = _parse_int_text(e, self._strictness)
				if v is None:
					wrong()
					return
				vals.append(v)
			if c.allowed is not None and c.allowed[0] == "ints":
				for i, v in enumerate(vals):
					if v not in c.allowed[1]:
						_vdiag(out, line, f"value not allowed at '{c.path}': {els[i].text}")
						break
			if c.min_i is not None and any(v < c.min_i for v in vals):
				_vdiag(out, line, f"value below min at '{c.path}'")
			if c.max_i is not None and any(v > c.max_i for v in vals):
				_vdiag(out, line, f"value above max at '{c.path}'")
		elif base == "float":
			vals = []
			for e in els:
				v = _parse_float_text(e, self._strictness)
				if v is None:
					wrong()
					return
				vals.append(v)
			if c.allowed is not None and c.allowed[0] == "floats":
				for i, v in enumerate(vals):
					if v not in c.allowed[1]:
						_vdiag(out, line, f"value not allowed at '{c.path}': {els[i].text}")
						break
			if c.min_f is not None and any(v < c.min_f for v in vals):
				_vdiag(out, line, f"value below min at '{c.path}'")
			if c.max_f is not None and any(v > c.max_f for v in vals):
				_vdiag(out, line, f"value above max at '{c.path}'")
		elif base == "bool":
			vals = []
			for e in els:
				v = _parse_bool_text(e.text, self._strictness)
				if v is None:
					wrong()
					return
				vals.append(v)
			if c.allowed is not None and c.allowed[0] == "bools":
				for i, v in enumerate(vals):
					if v not in c.allowed[1]:
						_vdiag(out, line, f"value not allowed at '{c.path}': {els[i].text}")
						break
		elif base == "datetime":
			vals = []
			for e in els:
				v = parse_datetime(e.text)
				if v is None:
					wrong()
					return
				vals.append(v)
			if c.allowed is not None and c.allowed[0] == "dates":
				for i, v in enumerate(vals):
					if not any(_dt_equal(v, a) for a in c.allowed[1]):
						_vdiag(out, line, f"value not allowed at '{c.path}': {els[i].text}")
						break
		else:
			# string kind or untyped: every element coerces; only the allowed
			# set can fail, in logical-string space.
			if c.allowed is not None and c.allowed[0] == "strings":
				for e in els:
					s = _apply_escapes(e.text)
					if s not in c.allowed[1]:
						_vdiag(out, line, f"value not allowed at '{c.path}': {s}")
						break

	def _v_unknown(self, sdef, out):
		# Unknown-field sweep: a schema path legalizes its name chain and every
		# prefix (selectors ignored). Only the topmost unknown node is
		# reported; its subtree is implied unknown and skipped.
		cons = sdef.cons
		# Chains below a fragment mount only match by descending the mounts.
		has_mounts = any(c.inherits is not None for c in cons)
		legal = set()
		# Sibling names per parent chain, built once (schema order): _v_suggest
		# used to rebuild every chain per unknown field, which bit hardest on
		# the wholesale-unmatched documents the feature exists for.
		siblings: dict = {}
		# Paths with a `*` segment can't live in the exact-chain hash; they
		# match element-wise (a star matches any one name, prefixes included).
		star_pats = []
		for c in cons:
			if any(s.star for s in c.segs):
				star_pats.append(c.segs)
			chain = ""
			for s in c.segs:
				if s.star:
					break   # no sibling entry for '*'; deeper chains are pattern-only
				siblings.setdefault(chain, []).append(s.name)
				chain = _chain_push(chain, s.name)
				legal.add(chain)
		stack = [(c, "", "") for c in reversed(self.arena[ROOT].children)]
		while stack:
			n, pchain, pshown = stack.pop()
			node = self.arena[n]
			chain = _chain_push(pchain, node.name)
			shown = node.name if not pshown else pshown + "." + node.name
			if (
				chain not in legal
				and not _star_legal(star_pats, chain)
				and not (has_mounts and _chain_legal(cons, sdef.frags, chain))
			):
				hint = _v_suggest(siblings, pchain, node.name)
				_vdiag(out, node.line, f"unknown field '{shown}'{hint}")
				continue
			for k in reversed(node.children):
				stack.append((k, chain, shown))


class StatusError(Exception):
	"""Raised by the must-exist convenience reads (get_* with no default): a
	public name a caller can actually catch. Carries the Status in .status."""

	def __init__(self, status):
		self.status = status
		super().__init__(status.name)


def _escape_name(name):
	"""Emit a stored (escape-resolved) name in a spelling that reads back as the
	same name: bare when it can be, else quoted with the escapes _apply_escapes
	undoes. This is a true inverse of the name parse, which _quote_text is not -
	that one picks a quote style to AVOID escaping and never escapes a
	backslash, which is right for a value (stored in its escaped spelling) and
	wrong for a name (stored resolved)."""
	# set(name) and the difference are both C-level; the generator this replaced
	# made one Python call per character of every name emitted.
	if name and not set(name) - _BARE_NAME_CHARS:
		return name
	out = ['"']
	for c in name:
		if c == "\\":
			out.append("\\\\")
		elif c == '"':
			out.append('\\"')
		elif c == "\t":
			out.append("\\t")
		elif c == "\n":
			out.append("\\n")
		else:
			out.append(c)
	out.append('"')
	return "".join(out)


def _emit_name(name):
	return _escape_name(name)


def quote_segment(name):
	"""Quote one path segment so it can be spliced into a lookup path: a bare
	name passes through, anything else comes back quoted and escaped in the
	form the path scanner accepts. Splicing user-typed text into a path
	without this is path injection - a dotted name silently reads as nesting.
	Same spelling paths() and the canonical emitter produce."""
	return _emit_name(name)


def _h001_head(name):
	"""The single H001 wording site: the hint builder and the schema suppressor
	both come here, so the suppressor matches the exact head the builder
	emitted - never a re-parse of free prose. (The leaf name cannot ride on
	Diagnostic itself: consumers build Diagnostic literals, so its field set
	is frozen.)"""
	return f"'{name}' repeats as a bare leaf - did you mean '{name}: "


def suppress_declared_repeats(schema, diags):
	"""Drop the H001 hints a schema disavows: a field whose declared repeat upper
	bound is above 1 repeats BY DESIGN (repetition is its instance mechanism),
	so the repeated-bare-leaf hint is structurally a false positive there and
	trains users to ignore hints. Matching is by leaf name - the filter
	consumers were hand-rolling - which errs toward quiet, for a hint. Used by
	`check --schema` and load_and_validate; call it wherever doc diagnostics
	and a schema meet. Mutates diags in place."""
	# Top-level fields plus every fragment's fields: a repeat declared inside
	# a mounted shape disavows the hint the same way.
	groups = [("field", schema.instances("field"))]
	for k in range(schema.count("fragment")):
		base = f"fragment[#{k}].field"
		groups.append((base, schema.instances(base)))
	names = []
	for base, paths in groups:
		for i, p in enumerate(paths):
			# repeat is a 1-2 element array (`repeat: lo[, hi]`); the bound
			# that matters here is the last one.
			rep = schema.read_int_array(f"{base}[#{i}].repeat")
			if rep.status != Status.Good:
				continue
			if not rep.value or rep.value[-1] <= 1:
				continue
			# Leaf name from the parsed path, not a re-split of its text: a
			# quoted last segment may contain dots (`a."b.c"`). The scanner
			# folds the name; the doc side stores names folded too.
			try:
				segments, _ = _scan_lookup(p)
			except _PathError:
				continue
			if not segments:
				continue
			seg = segments[-1]
			if seg.star:
				continue   # name wildcard: no single leaf name to disavow
			names.append(seg.name)
	if not names:
		return
	heads = [_h001_head(n) for n in names]
	kept = []
	for d in diags:
		if d.code == "H001" and any(d.message.startswith(h) for h in heads):
			continue
		kept.append(d)
	diags[:] = kept


class FileStatus(Enum):
	"""What load_file found: the four cases a consumer's own load path
	otherwise confuses. Clean and HadErrors both carry a usable document;
	NotFound and Unreadable come back with an empty one."""
	Clean = 0        # read and parsed, no error diagnostics (hints allowed)
	HadErrors = 1    # read and parsed, but error diagnostics are present
	NotFound = 2     # no file at the path
	Unreadable = 3   # exists but could not be read (permissions, a directory, bad encoding)


def _publish_file(tmp, target):
	# Move the finished temp file over the target. On windows that means
	# ReplaceFile rather than a rename: a rename publishes a brand-new file and
	# leaves the destination's ACLs, attributes and named streams behind, which
	# ReplaceFile carries onto the replacement instead. It needs the destination
	# to exist, and it fails rather than skip a merge it cannot do (no WRITE_DAC,
	# say), so a create and any failure fall back to os.replace.
	if os.name == "nt" and os.path.exists(target):
		import ctypes

		REPLACEFILE_WRITE_THROUGH = 0x1
		# WinDLL exists only on windows, and mypy checks this file against the
		# POSIX stubs, where the name is simply absent.
		k32 = ctypes.WinDLL("kernel32", use_last_error=True)  # type: ignore[attr-defined]
		k32.ReplaceFileW.restype = ctypes.c_int
		k32.ReplaceFileW.argtypes = [
			ctypes.c_wchar_p,
			ctypes.c_wchar_p,
			ctypes.c_wchar_p,
			ctypes.c_ulong,
			ctypes.c_void_p,
			ctypes.c_void_p,
		]
		if k32.ReplaceFileW(target, tmp, None, REPLACEFILE_WRITE_THROUGH, None, None):
			return
	os.replace(tmp, target)


def _sync_dir(d):
	# The rename is a directory change, and the fsync on the file only covered
	# the file: without this a power cut right after a save can lose the publish
	# and leave the old content. Best effort - windows has no directory fsync,
	# and a filesystem that refuses one is not a reason to fail a write that
	# already succeeded.
	try:
		fd = os.open(d, os.O_RDONLY)
	except OSError:
		return
	try:
		os.fsync(fd)
	except OSError:
		pass
	finally:
		os.close(fd)


def write_file_atomic(file, data):
	# The file tier's write mechanism (also what the CLI's --write uses): a
	# temp file in the same dir, then a rename over the target,
	# so an interrupted write can never truncate the config it rewrites. The data
	# is synced before the rename so a crash cannot publish an empty file.
	# Returns None on success, or the error message to report.
	#
	# A rename publishes a new inode, so the target is resolved through symlinks
	# first (otherwise a linked-in config gets replaced by a regular file and the
	# real one is left stale) and the original's mode is copied onto the temp file
	# (otherwise a 600 config comes back at whatever the umask allows). Other hard
	# links to the old inode cannot survive a rename and keep the old content.
	# A NUL in the path raises ValueError rather than OSError, and this call
	# promises a returned message, never a throw. POSIX raises it here, at the
	# resolve; windows resolves such a path happily and raises at the first call
	# that touches the filesystem instead, so every one of them below has to
	# carry the same guard.
	try:
		target = os.path.realpath(file)
	except ValueError as e:
		return f"{file}: {e}"
	d = os.path.dirname(target)
	if d == "":
		d = "."
	base = os.path.basename(target)
	if base == "":
		base = target
	# Exclusive create: the name is predictable, so anything already sitting
	# there - including a symlink someone else planted - must make this fail
	# rather than be written through. Retry past a stale collision, then give
	# up; refusing to write beats writing somewhere unintended.
	# A file that already exists keeps its own mode, so its temp is born private
	# and the real mode goes on below - the copy is never briefly readable to
	# anyone the original was not. A file that does not exist yet has no mode to
	# preserve, so it takes the one an ordinary create would: 0666 narrowed by
	# the umask, like every other file the user's tools produce.
	try:
		existing = os.stat(target)
	except (OSError, ValueError):
		existing = None
	born = 0o600 if existing is not None else 0o666
	f = None
	tmp = ""
	last = ""
	for attempt in range(8):
		tmp = os.path.join(d, f".{base}.tmp{os.getpid()}.{attempt}")
		try:
			fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, born)
			f = os.fdopen(fd, "w", encoding="utf-8", newline="")
			break
		except (OSError, ValueError) as e:
			last = str(e)
	if f is None:
		return f"{file}: cannot create temporary file: {last}"
	try:
		try:
			# On the handle, so umask cannot narrow it the way it narrows a
			# create mode. Best effort: a filesystem that cannot carry the mode
			# is not a reason to fail a write that otherwise succeeded. fchmod
			# is POSIX-only, and so is the mode concept it copies - on windows
			# the destination's attributes come across in the publish step
			# instead, and calling it there would raise past this contract.
			if existing is not None and hasattr(os, "fchmod"):
				try:
					os.fchmod(f.fileno(), stat.S_IMODE(existing.st_mode))
				except OSError:
					pass
			f.write(data)
			f.flush()
			os.fsync(f.fileno())
		finally:
			f.close()
	except OSError as e:
		try:
			os.remove(tmp)
		except OSError:
			pass
		return f"{file}: {e}"
	try:
		_publish_file(tmp, target)
	except OSError as e:
		try:
			os.remove(tmp)
		except OSError:
			pass
		return f"{file}: {e}"
	_sync_dir(d)
	return None


def _h002_head(name):
	"""The single H002 wording site: the merge hint and the schema suppressor
	both come here, same discipline as _h001_head."""
	return f"merged with '{name}' at "


def suppress_declared_reopens(schema, diags):
	"""Drop the H002 hints a schema disavows: a section whose entry declares
	`reopen: true` is MEANT to be written in parts, so the merge hint is
	structurally a false positive there. Matching is by leaf name, same as the
	H001 suppressor, and it errs toward quiet, for a hint. Used by
	`check --schema` and load_and_validate; call it wherever doc diagnostics
	and a schema meet. Mutates diags in place."""
	groups = [("field", schema.instances("field"))]
	for k in range(schema.count("fragment")):
		base = f"fragment[#{k}].field"
		groups.append((base, schema.instances(base)))
	names = []
	for base, paths in groups:
		for i, p in enumerate(paths):
			re = schema.read_bool(f"{base}[#{i}].reopen")
			if re.status != Status.Good or not re.value:
				continue
			try:
				segments, _ = _scan_lookup(p)
			except _PathError:
				continue
			if not segments:
				continue
			seg = segments[-1]
			if seg.star:
				continue   # name wildcard: no single leaf name to disavow
			names.append(seg.name)
	if not names:
		return
	heads = [_h002_head(n) for n in names]
	kept = []
	for d in diags:
		if d.code == "H002" and any(d.message.startswith(h) for h in heads):
			continue
		kept.append(d)
	diags[:] = kept


_RESERVED = frozenset(" \t,:#\"'[]")


def _emit_element(e):
	"""Minimal quoting: bare unless a reserved character (or lookalike hazard) forces it.

	One addition: an author-quoted element keeps its quotes unless the text reads as
	one of SHCL's own data formats - quoting those is just spelling (readers type the
	value either way), but quoting a plain string is the escape and must survive
	canonicalization. This clause only ever adds quoting, so a bare emit stays safe.
	"""
	t = e.text
	# isdisjoint iterates the text in C and stops at the first hit; the generator
	# it replaced made one Python call per character of every element emitted.
	needs = (not t) or not _RESERVED.isdisjoint(t) or (_fence_open(t) is not None)
	# Edge whitespace beyond the space/tab in _RESERVED still has to force quotes:
	# the parser trims the full White_Space set, so a bare NBSP (or VT, FF, NEL,
	# ideographic space) at either end would not survive the reload. Edges only -
	# interior whitespace is never trimmed and quoting it would move bytes.
	if not needs and t and (t[0] in _WS_SET or t[-1] in _WS_SET):
		needs = True
	if not needs and e.quoted and not _is_data_format(e):
		needs = True
	return _quote_text(t) if needs else t


def _is_data_format(e):
	"""True when the text reads as an int, float, bool, or datetime at standard
	strictness - fixed there deliberately, so canonical form cannot vary with
	the load strictness.

	One pass over the text before any coercion: at Standard the int, float and
	datetime forms all require at least one ASCII digit, and the only formats
	that do not are the boolean words, the longest of which is "false". An
	ordinary quoted string fails both tests, so emit stops running four full
	coercions on every quoted element it writes."""
	if not any("0" <= c <= "9" for c in e.text):
		t = _trim(e.text)
		return len(t) <= 5 and _parse_bool_text(t, Strictness.Standard) is not None
	if _parse_int_text(e, Strictness.Standard) is not None:
		return True
	if _parse_float_text(e, Strictness.Standard) is not None:
		return True
	if _parse_bool_text(e.text, Strictness.Standard) is not None:
		return True
	return parse_datetime(e.text) is not None


def _bare_quote_counts(t):
	"""Count quote chars that are NOT already escaped; escaped ones stay untouched
	or every round-trip would re-escape them."""
	dq = sq = 0
	it = iter(t)
	for c in it:
		if c == "\\":
			next(it, None)
		elif c == '"':
			dq += 1
		elif c == "'":
			sq += 1
	return dq, sq


def _quote_text(t):
	# A dangling trailing backslash would turn the closing quote into an
	# escape pair - the scanner reads the path back wrong, or not at all.
	# Store the doubled spelling (identical on string read), the same rule
	# the element parser applies to bare text.
	if t.endswith("\\"):
		t = _normalize_dangling_backslash(t)
	dq, sq = _bare_quote_counts(t)
	if dq == 0:
		return '"' + t + '"'
	if sq == 0:
		return "'" + t + "'"
	# Both quote kinds appear bare: escape the doubles, wrap in doubles.
	out = ['"']
	it = iter(t)
	for c in it:
		if c == "\\":
			out.append(c)
			nxt = next(it, None)
			if nxt is not None:
				out.append(nxt)
		elif c == '"':
			out.append('\\"')
		else:
			out.append(c)
	out.append('"')
	return "".join(out)


# ---------------------------------------------------------------------------
# Coercion ("intelligent but safe"; Loose re-admits a closed list of tricks)
# ---------------------------------------------------------------------------

_CURRENCY = set("$¢£¤¥₩₪₫€₭₮₱₲₴₹₺₼₽₾₿")

_I64_MIN = -(2 ** 63)
_I64_MAX = 2 ** 63 - 1


def _strip_currency(t):
	if t and t[0] in _CURRENCY:
		return t[1:]
	return t


def _parse_i64(t):
	# Rust t.parse::<i64>(): optional +/- then ASCII digits, range-checked.
	body = t[1:] if t[:1] in ("+", "-") else t
	if not body or not all(_is_ascii_digit(c) for c in body):
		return None
	# Length-gate before int(): CPython 3.11+ refuses >4300 decimal digits, but the
	# reference just overflows. Leading zeros are legal and don't count toward range.
	digits = body.lstrip("0") or "0"
	if len(digits) > 19:
		return None
	n = -int(digits) if t[:1] == "-" else int(digits)
	if n < _I64_MIN or n > _I64_MAX:
		return None
	return n


def _rust_round(f):
	# Nearest integer, ties away from zero (matches Rust f64::round). Returns int.
	fl = math.floor(f)
	diff = f - fl
	if diff < 0.5:
		return fl
	if diff > 0.5:
		return fl + 1
	return fl + 1 if f > 0 else fl


def _parse_int_text(e, level):
	t = _trim(e.text)
	if level == Strictness.Loose:
		t = _strip_currency(t)
	# Plain decimal.
	body = t[1:] if t[:1] in ("+", "-") else t
	if body and all(_is_ascii_digit(c) for c in body):
		return _parse_i64(t)
	# Hex.
	if t[:1] == "-":
		neg = True
		hexs = t[1:]
	else:
		neg = False
		hexs = t[1:] if t[:1] == "+" else t
	if hexs[:2] in ("0x", "0X"):
		h = hexs[2:]
		if h and all(c in "0123456789abcdefABCDEF" for c in h):
			# Range-check the magnitude against the sign, so the negative i64-min
			# magnitude (0x8000000000000000) reads like its decimal spelling.
			mag = int(h, 16)
			if mag > (_I64_MAX + 1 if neg else _I64_MAX):
				return None
			return -mag if neg else mag
	# Thousands separators, only inside quotes (bare commas are reserved).
	if e.quoted and "," in t:
		sign_body = t[1:] if t[:1] in ("+", "-") else t
		groups = sign_body.split(",")
		well_formed = (
			len(groups) > 1
			and groups[0] != ""
			and len(groups[0]) <= 3
			and all(_is_ascii_digit(c) for c in groups[0])
			and all(len(g) == 3 and all(_is_ascii_digit(c) for c in g) for g in groups[1:])
		)
		if well_formed:
			return _parse_i64(t.replace(",", ""))
	# Loose: a float (including %) rounds, half away from zero.
	if level == Strictness.Loose:
		f = _parse_float_text(e, level)
		if f is not None and not math.isnan(f) and not math.isinf(f):
			r = _rust_round(f)
			if -(2 ** 63) <= r <= 2 ** 63:
				if r > _I64_MAX:
					r = _I64_MAX
				elif r < _I64_MIN:
					r = _I64_MIN
				return r
	return None


def _float_shape_ok(t):
	body = t[1:] if t[:1] in ("+", "-") else t
	if not body:
		return False
	epos = -1
	for i, c in enumerate(body):
		if c == "e" or c == "E":
			epos = i
			break
	if epos >= 0:
		mantissa = body[:epos]
		exp = body[epos + 1:]
	else:
		mantissa = body
		exp = None
	if exp is not None:
		xb = exp[1:] if exp[:1] in ("+", "-") else exp
		if not xb or not all(_is_ascii_digit(c) for c in xb):
			return False
	dot = mantissa.find(".")
	if dot >= 0:
		int_part = mantissa[:dot]
		frac_part = mantissa[dot + 1:]
	else:
		int_part = mantissa
		frac_part = ""
	if int_part == "" and frac_part == "":
		return False
	return all(_is_ascii_digit(c) for c in int_part) and all(_is_ascii_digit(c) for c in frac_part)


def _parse_float_text(e, level):
	t = _trim(e.text)
	percent = False
	if level == Strictness.Loose:
		t = _strip_currency(t)
		if t.endswith("%"):
			t = _trim_end(t[:-1])
			percent = True
	if _float_shape_ok(t):
		try:
			v = float(t)
		except ValueError:
			return None
	else:
		# An integer is a valid float on read (incl. hex and quoted thousands).
		iv = _parse_int_text_no_loose(_Element(t, e.quoted))
		if iv is None:
			return None
		v = float(iv)
	return v / 100.0 if percent else v


def _parse_int_text_no_loose(e):
	# Integer forms only (no Loose float fallback) - used by the float path.
	return _parse_int_text(e, Strictness.Standard)


def _parse_bool_text(t, level):
	s = _ascii_lower(_trim(t))
	if s == "true":
		return True
	if s == "false":
		return False
	if level == Strictness.Strict:
		return None
	if s in ("yes", "on", "1"):
		return True
	if s in ("no", "off", "0"):
		return False
	if level == Strictness.Loose:
		if s in ("t", "y", "enable", "enabled"):
			return True
		if s in ("f", "n", "disable", "disabled"):
			return False
	return None


# ---------------------------------------------------------------------------
# Date/time (closed whitelist; shape match, then calendar validation)
# ---------------------------------------------------------------------------

_MONTHS = {
	"jan": 1, "feb": 2, "mar": 3, "apr": 4, "may": 5, "jun": 6,
	"jul": 7, "aug": 8, "sep": 9, "oct": 10, "nov": 11, "dec": 12,
	"january": 1, "february": 2, "march": 3, "april": 4, "june": 6,
	"july": 7, "august": 8, "september": 9, "october": 10,
	"november": 11, "december": 12,
}


def _month_from_name(s):
	return _MONTHS.get(_ascii_lower(s))


def _days_in_month(y, m):
	if m in (1, 3, 5, 7, 8, 10, 12):
		return 31
	if m in (4, 6, 9, 11):
		return 30
	if m == 2:
		return 29 if (y % 4 == 0 and y % 100 != 0) or y % 400 == 0 else 28
	return 0


def _valid_date(y, m, d):
	return 1 <= m <= 12 and d >= 1 and d <= _days_in_month(y, m)


def _parse_u32(s):
	# Rust u32 parse: optional leading '+', ASCII digits, range-checked.
	if not s:
		return None
	body = s[1:] if s[0] == "+" else s
	if not body or not all(_is_ascii_digit(c) for c in body):
		return None
	# Length-gate before int(): CPython 3.11+ refuses >4300 decimal digits, but the
	# reference just overflows. Leading zeros are legal and don't count toward range.
	digits = body.lstrip("0") or "0"
	if len(digits) > 10:
		return None
	n = int(digits)
	if n > 2 ** 32 - 1:
		return None
	return n


def _parse_year4(s):
	if len(s) == 4 and all(_is_ascii_digit(c) for c in s):
		return int(s)
	return None


def _parse_num2(s):
	if (len(s) == 1 or len(s) == 2) and all(_is_ascii_digit(c) for c in s):
		return int(s)
	return None


def _parse_date_part(s):
	s = _trim(s)
	# Compact 8-digit YYYYMMDD.
	if len(s) == 8 and all(_is_ascii_digit(c) for c in s):
		y = int(s[:4])
		m = int(s[4:6])
		d = int(s[6:8])
		return (y, m, d) if _valid_date(y, m, d) else None
	# Space-separated named-month forms; a comma may follow the day in "Mon DD, YYYY".
	toks = _split_ws(s)
	if len(toks) == 3:
		m = _month_from_name(toks[0])
		if m is not None:
			day_tok = toks[1][:-1] if toks[1].endswith(",") else toks[1]
			d = _parse_u32(day_tok)
			y = _parse_year4(toks[2])
			if d is None or y is None:
				return None
			return (y, m, d) if _valid_date(y, m, d) else None
		m = _month_from_name(toks[1])
		if m is not None:
			d = _parse_u32(toks[0])
			y = _parse_year4(toks[2])
			if d is None or y is None:
				return None
			return (y, m, d) if _valid_date(y, m, d) else None
		return None
	if len(toks) != 1:
		return None
	# Delimited forms: one of - / . used uniformly.
	delim = None
	for c in s:
		if c == "-" or c == "/" or c == ".":
			delim = c
			break
	if delim is None:
		return None
	parts = s.split(delim)
	if len(parts) != 3 or any(p == "" for p in parts):
		return None
	# The delimiter must be uniform: no other delimiter chars anywhere.
	if sum(1 for c in s if c == "-" or c == "/" or c == ".") != 2:
		return None
	if len(parts[0]) == 4 and all(_is_ascii_digit(c) for c in parts[0]):
		y = int(parts[0])
		m = _parse_num2(parts[1])
		d = _parse_num2(parts[2])
		if m is None or d is None:
			return None
		return (y, m, d) if _valid_date(y, m, d) else None
	m = _month_from_name(parts[0])
	if m is not None:
		d = _parse_num2(parts[1])
		y = _parse_year4(parts[2])
		if d is None or y is None:
			return None
		return (y, m, d) if _valid_date(y, m, d) else None
	m = _month_from_name(parts[1])
	if m is not None:
		d = _parse_num2(parts[0])
		y = _parse_year4(parts[2])
		if d is None or y is None:
			return None
		return (y, m, d) if _valid_date(y, m, d) else None
	return None   # everything else (MM/DD/YYYY, 2-digit years, epoch) is rejected


def _parse_time_part(s):
	"""Time with optional meridiem, fraction, zone: `H:MM[:SS[.f+]][ AM|PM][Z|+HH:MM]`.
	Returns ((h, mi, sec-if-written), frac-or-None, zone-or-None) or None."""
	t = _trim(s)
	# Zone suffix first (only valid after a time).
	zone = None
	if t and (t[-1] == "Z" or t[-1] == "z"):
		zone = ("utc", None)
		t = _trim_end(t[:-1])
	elif len(t) >= 6:
		tail = t[-6:]
		sign = tail[0]
		if (sign == "+" or sign == "-") and _is_ascii_digit(tail[1]) and _is_ascii_digit(tail[2]) \
				and tail[3] == ":" and _is_ascii_digit(tail[4]) and _is_ascii_digit(tail[5]):
			hh = int(tail[1:3])
			mm = int(tail[4:6])
			if hh <= 23 and mm <= 59:
				off = hh * 60 + mm
				if sign == "-":
					off = -off
				zone = ("offset", off)
				t = _trim_end(t[:-6])
	# Meridiem: dotted a.m. is rejected (the '.' fails the digit checks below).
	meridiem = None   # True = PM
	lower = _ascii_lower(t)
	if lower.endswith("am"):
		meridiem = False
		t = t[:len(_trim_end(lower[:-2]))]
	elif lower.endswith("pm"):
		meridiem = True
		t = t[:len(_trim_end(lower[:-2]))]
	t = _trim_end(t)
	# Fraction: only after seconds, '.' delimiter, 1-9 digits.
	dot = t.find(".")
	if dot >= 0:
		hms = t[:dot]
		f = t[dot + 1:]
		if f == "" or len(f) > 9 or not all(_is_ascii_digit(c) for c in f):
			return None
		frac = f
	else:
		hms = t
		frac = None
	parts = hms.split(":")
	if len(parts) < 2 or len(parts) > 3:
		return None
	if frac is not None and len(parts) != 3:
		return None   # fraction can only follow HH:MM:SS
	h_raw = _parse_num2(parts[0])
	if h_raw is None:
		return None
	if len(parts[1]) != 2:
		return None
	mi = _parse_num2(parts[1])
	if mi is None:
		return None
	if len(parts) == 3:
		if len(parts[2]) != 2:
			return None
		sec = _parse_num2(parts[2])
		if sec is None:
			return None
	else:
		sec = None
	if mi > 59 or (sec is not None and sec > 59):
		return None
	if meridiem is None:
		if h_raw > 23:
			return None
		h = h_raw
	else:
		if not (1 <= h_raw <= 12):
			return None
		if not meridiem and h_raw == 12:
			h = 0
		elif not meridiem:
			h = h_raw
		elif h_raw == 12:
			h = 12
		else:
			h = h_raw + 12
	return ((h, mi, sec), frac, zone)


def parse_datetime(text):
	"""Whole-value date/time parse per the whitelist. None = BadType."""
	t = _trim(text)
	if not t:
		return None
	colon = t.find(":")
	if colon != -1:
		# Scan back over the 1-2 hour digits to find where the time starts.
		k = colon
		while k > 0 and _is_ascii_digit(t[k - 1]) and colon - k < 2:
			k -= 1
		if k == colon:
			return None   # ':' with no hour digits before it
		if k == 0:
			# Time-only value.
			tp = _parse_time_part(t)
			if tp is None:
				return None
			(h, mi, s), frac, zone = tp
			return ShclDateTime(date=None, time=(h, mi, s), frac=frac, zone=zone)
		# Combined: one separator char between date and time.
		sep = t[k - 1]
		if sep not in ("T", "t", " ", "_", "-", "/", "."):
			return None
		date = _parse_date_part(t[:k - 1])
		if date is None:
			return None
		tp = _parse_time_part(t[k:])
		if tp is None:
			return None
		(h, mi, s), frac, zone = tp
		return ShclDateTime(date=date, time=(h, mi, s), frac=frac, zone=zone)
	# Date-only.
	date = _parse_date_part(t)
	if date is None:
		return None
	return ShclDateTime(date=date, time=None, frac=None, zone=None)


# ---------------------------------------------------------------------------
# Validator: schema build helpers (module level; methods live on Document)
# ---------------------------------------------------------------------------

_SCHEMA_TYPES = (
	"int", "float", "bool", "string", "datetime", "raw",
	"int-array", "float-array", "bool-array", "string-array", "datetime-array",
)


class _Constraint:
	__slots__ = (
		"path", "segs", "ty", "required", "allowed",
		"min_i", "max_i", "min_f", "max_f", "repeat",
		"inherits", "inherits_line",
		"desc", "default_text",
	)

	def __init__(self, path, segs):
		self.path = path          # as written in the schema; message text only
		self.segs = segs
		self.ty = None            # member of _SCHEMA_TYPES
		self.required = False
		self.allowed = None       # ("ints"|"floats"|"bools"|"dates"|"strings", list)
		self.min_i = None
		self.max_i = None
		self.min_f = None
		self.max_f = None
		self.repeat = None        # (lo, hi)
		self.inherits = None      # fragment mounted at this path (subtree shape)
		self.inherits_line = 0    # schema line of the `inherits` key, for V095
		# Generator-only (`shcl init`): validation ignores both.
		self.desc = None          # `desc`, a one-line description
		self.default_text = None  # `default`, emitted as an inline value

	def clone(self):
		cc = _Constraint(self.path, list(self.segs))
		cc.ty = self.ty
		cc.required = self.required
		cc.allowed = self.allowed
		cc.min_i = self.min_i
		cc.max_i = self.max_i
		cc.min_f = self.min_f
		cc.max_f = self.max_f
		cc.repeat = self.repeat
		cc.inherits = self.inherits
		cc.inherits_line = self.inherits_line
		cc.desc = self.desc
		cc.default_text = self.default_text
		return cc


class _SchemaDef:
	# An interpreted schema: the top-level constraints plus the named fragments
	# their `inherits` keys can mount. paths_complete is False when a fault
	# cost the schema a path spelling (unreadable `field:` path, or a mount
	# naming no declared fragment); key-level faults keep their entry's chain,
	# so only those two classes can turn declared fields into false unknowns -
	# the sweep runs unless one of them happened.
	__slots__ = ("cons", "frags", "paths_complete")

	def __init__(self, cons, frags, paths_complete):
		self.cons = cons
		self.frags = frags        # name -> list of _Constraint
		self.paths_complete = paths_complete


def _vdiag(out, line, msg):
	out.append(Diagnostic(line, Severity.Error, msg, _diag_code(msg)))


def _single_text(v):
	# One scalar constraint value (escapes applied), or None for anything else.
	if v.kind == "cell" and len(v.els) == 1:
		return _apply_escapes(v.els[0].text)
	return None


def _dt_equal(a, b):
	# Field-wise; tuples/None compare by value already.
	return a.date == b.date and a.time == b.time and a.frac == b.frac and a.zone == b.zone


def _build_schema(schema):
	"""Interpret a parsed schema document into constraints and fragments, plus
	any schema faults (V09x, schema-file lines). Whatever parsed cleanly is
	kept even when faults are present - a broken key drops that key, a broken
	field drops that field - so a caller can still check the document against
	the surviving constraints."""
	faults: list = []
	cons = []
	frags = {}
	paths_complete = True
	for f in schema.arena[ROOT].children:
		node = schema.arena[f]
		if node.name == "field":
			c = _parse_field(schema, f, faults)
			if c is not None:
				cons.append(c)
			else:
				paths_complete = False
		elif node.name == "fragment":
			name = _single_text(node.value)
			if not name:
				_vdiag(faults, node.line, "bad schema fragment")
				continue
			if name in frags:
				_vdiag(faults, node.line, f"bad schema fragment '{name}': duplicate")
				continue
			fcs = []
			for k in schema.arena[f].children:
				kid = schema.arena[k]
				if kid.name == "field":
					c = _parse_field(schema, k, faults)
					if c is not None:
						fcs.append(c)
					else:
						paths_complete = False
				else:
					_vdiag(faults, kid.line, f"bad schema fragment '{name}': unknown key '{kid.name}'")
			frags[name] = fcs
		else:
			_vdiag(faults, node.line, f"unknown schema key '{node.name}'")
	# Every mount must name a declared fragment; cycles (self or mutual) are
	# legal - expansion is demand-driven against a finite document.
	for c in cons + [fc for fcs in frags.values() for fc in fcs]:
		if c.inherits is not None and c.inherits not in frags:
			_vdiag(faults, c.inherits_line, f"unknown schema fragment '{c.inherits}'")
			paths_complete = False
	# One constraint per line in practice, so line order = file order.
	faults.sort(key=lambda d: d.line)
	return _SchemaDef(cons, frags, paths_complete), faults


def _parse_field(schema, f, faults):
	"""One `field:` instance (top-level or inside a fragment) -> a _Constraint.
	None = faults were reported and the constraint is dropped."""
	node = schema.arena[f]
	path = _single_text(node.value)
	if path is None:
		_vdiag(faults, node.line, "bad schema path")
		return None
	try:
		segs, value_text = _scan_lookup(path)
	except _PathError:
		segs, value_text = None, None
	if segs is None or value_text is not None:
		_vdiag(faults, node.line, f"bad schema path: {path}")
		return None
	c = _Constraint(path, segs)
	# Deferred so `min: 1` may precede `type: int` in the file.
	required = None
	reopen_seen = False
	allowed_at = None
	min_at = None
	max_at = None
	for k in schema.arena[f].children:
		kid = schema.arena[k]
		if kid.value.is_empty():
			continue  # dangling key: treated as absent
		if kid.name == "type":
			t = _single_text(kid.value)
			if t is not None:
				t = _ascii_lower(t)
			if t in _SCHEMA_TYPES:
				if c.ty is not None:
					_vdiag(faults, kid.line, "bad schema constraint 'type'")
				else:
					c.ty = t
			elif t is not None:
				_vdiag(faults, kid.line, f"unknown schema type '{t}'")
			else:
				_vdiag(faults, kid.line, "bad schema constraint 'type'")
		elif kid.name == "required":
			t = _single_text(kid.value)
			b = _parse_bool_text(t, Strictness.Standard) if t is not None else None
			if b is not None and required is None:
				required = b
			else:
				_vdiag(faults, kid.line, "bad schema constraint 'required'")
		elif kid.name == "reopen":
			# Consumed by the H002 suppressor (which reads the schema document
			# directly); validation itself ignores it, but a bad value still
			# faults so a typo cannot silently disavow nothing.
			t = _single_text(kid.value)
			b = _parse_bool_text(t, Strictness.Standard) if t is not None else None
			if b is not None and not reopen_seen:
				reopen_seen = True
			else:
				_vdiag(faults, kid.line, "bad schema constraint 'reopen'")
		elif kid.name == "allowed":
			if kid.value.kind == "cell" and allowed_at is None:
				allowed_at = k
			else:
				_vdiag(faults, kid.line, "bad schema constraint 'allowed'")
		elif kid.name == "min":
			if kid.value.kind == "cell" and len(kid.value.els) == 1 and min_at is None:
				min_at = k
			else:
				_vdiag(faults, kid.line, "bad schema constraint 'min'")
		elif kid.name == "max":
			if kid.value.kind == "cell" and len(kid.value.els) == 1 and max_at is None:
				max_at = k
			else:
				_vdiag(faults, kid.line, "bad schema constraint 'max'")
		elif kid.name == "repeat":
			if kid.value.kind == "cell" and c.repeat is None and len(kid.value.els) in (1, 2):
				lo = _parse_uint(kid.value.els[0].text)
				hi = _parse_uint(kid.value.els[-1].text)
				if lo is not None and hi is not None and lo <= hi:
					c.repeat = (lo, hi)
				else:
					_vdiag(faults, kid.line, "bad schema constraint 'repeat'")
			else:
				_vdiag(faults, kid.line, "bad schema constraint 'repeat'")
		elif kid.name == "inherits":
			t = _single_text(kid.value)
			if t and c.inherits is None:
				c.inherits = t
				c.inherits_line = kid.line
			else:
				_vdiag(faults, kid.line, "bad schema constraint 'inherits'")
		elif kid.name == "desc":
			# Generator-only (`shcl init`); validation ignores it. First wins.
			if c.desc is None:
				c.desc = _single_text(kid.value)
		elif kid.name == "default":
			if c.default_text is None:
				c.default_text = _emit_value_inline(kid.value)
		else:
			_vdiag(faults, kid.line, f"unknown schema key '{kid.name}'")
	if required is not None:
		c.required = required
	base = c.ty[:-6] if c.ty is not None and c.ty.endswith("-array") else c.ty
	if base is None:
		base = "string"
	if allowed_at is not None:
		kid = schema.arena[allowed_at]
		els = kid.value.els
		# Schema values are read at Standard; only the document's values
		# coerce at the document's strictness.
		ok = True
		if base == "int":
			vals = [_parse_int_text(e, Strictness.Standard) for e in els]
			ok = all(v is not None for v in vals)
			setv = ("ints", vals)
		elif base == "float":
			vals = [_parse_float_text(e, Strictness.Standard) for e in els]
			ok = all(v is not None for v in vals)
			setv = ("floats", vals)
		elif base == "bool":
			vals = [_parse_bool_text(e.text, Strictness.Standard) for e in els]
			ok = all(v is not None for v in vals)
			setv = ("bools", vals)
		elif base == "datetime":
			vals = [parse_datetime(e.text) for e in els]
			ok = all(v is not None for v in vals)
			setv = ("dates", vals)
		elif base == "raw":
			ok = False  # a raw body has no element space to enumerate
			setv = None
		else:
			setv = ("strings", [_apply_escapes(e.text) for e in els])
		if ok:
			c.allowed = setv
		else:
			_vdiag(faults, kid.line, "bad schema constraint 'allowed'")
	for at, is_min in ((min_at, True), (max_at, False)):
		if at is None:
			continue
		kid = schema.arena[at]
		el = kid.value.els[0]
		key = "min" if is_min else "max"
		if base == "int":
			v = _parse_int_text(el, Strictness.Standard)
			if v is None:
				_vdiag(faults, kid.line, f"bad schema constraint '{key}'")
			elif is_min:
				c.min_i = v
			else:
				c.max_i = v
		elif base == "float":
			v = _parse_float_text(el, Strictness.Standard)
			if v is None:
				_vdiag(faults, kid.line, f"bad schema constraint '{key}'")
			elif is_min:
				c.min_f = v
			else:
				c.max_f = v
		else:
			_vdiag(faults, kid.line, f"bad schema constraint '{key}'")
	return c


def _emit_value_inline(v):
	# Re-emit a schema `default`/`allowed` value as an inline value (minimal
	# quoting, array elements joined by ", "). None for empty or raw - neither has
	# a usable one-line form. Used by the generator, not the validator.
	if v.kind != "cell":
		return None
	return ", ".join(_emit_element(e) for e in v.els)


def _allowed_join(a):
	kind, items = a
	if kind == "ints":
		return ", ".join(str(x) for x in items)
	if kind == "floats":
		return ", ".join(format_float(x) for x in items)
	if kind == "bools":
		return ", ".join("true" if x else "false" for x in items)
	if kind == "dates":
		return ", ".join(str(x) for x in items)
	return ", ".join(items)  # strings


def _gen_annotation(c, tyname):
	# The `# type, ...` line summarizing a constraint, ASCII only.
	parts = [tyname]
	if c.allowed is not None:
		parts.append("one of: " + _allowed_join(c.allowed))
	elif c.min_i is not None or c.max_i is not None:
		if c.min_i is not None and c.max_i is not None:
			parts.append(f"{c.min_i}-{c.max_i}")
		elif c.min_i is not None:
			parts.append(f">= {c.min_i}")
		else:
			parts.append(f"<= {c.max_i}")
	elif c.min_f is not None or c.max_f is not None:
		if c.min_f is not None and c.max_f is not None:
			parts.append(format_float(c.min_f) + "-" + format_float(c.max_f))
		elif c.min_f is not None:
			parts.append(">= " + format_float(c.min_f))
		else:
			parts.append("<= " + format_float(c.max_f))
	if c.repeat is not None:
		lo, hi = c.repeat
		parts.append(f"repeat {lo}" if lo == hi else f"repeat {lo}-{hi}")
	if c.required:
		parts.append("required")
	return ", ".join(parts)


def _gen_default_text(v):
	# A default carrying a literal newline cannot sit on a value line; the
	# quoted escaped spelling reads back to the same string.
	if "\n" not in v:
		return v
	s = ['"']
	for ch in v:
		if ch == "\\":
			s.append("\\\\")
		elif ch == '"':
			s.append('\\"')
		elif ch == "\n":
			s.append("\\n")
		elif ch == "\t":
			s.append("\\t")
		else:
			s.append(ch)
	s.append('"')
	return "".join(s)


def generate(schema, no_banner=False):
	"""Emit a commented, typed starter config from a schema (`shcl init
	--schema`). Paths that must exist (required, or a repeat lower bound of 1+)
	are live (their `default`, or an empty value); optional paths are commented
	out so the file is valid and minimal as-is. A must-exist wildcard path
	whose parent gets materialized by another live line is generated too, in
	dotted form - otherwise the file would fail the very schema that produced
	it - and remaining wildcard or `[#N]` paths (which cannot be materialized)
	are listed in a trailing comment block. The output always loads clean and
	validates clean against its schema, except a repeat lower bound of 2+
	(identical generated lines would merge, so the shortfall is reported). A
	footer naming the format and pointing at the spec is written last unless
	no_banner; the flag is negative so leaving it alone writes the footer.
	Returns (text, faults): a non-empty fault list (V09x) means the schema is
	broken and text is empty."""
	# Generation lays the whole schema out, so unlike validation it has no
	# safe partial mode: any fault fails it.
	sdef, faults = _build_schema(schema)
	if faults:
		return "", faults
	cons, cuts = _expand_mounts(sdef)
	if len(cons) >= _GEN_MAX_FIELDS:
		faults = []
		_vdiag(faults, 0, f"schema expands past {_GEN_MAX_FIELDS} fields; fragments mounted at more than one path multiply")
		return "", faults

	def must_exist(c):
		return c.required or (c.repeat is not None and c.repeat[0] >= 1)

	def has_wild(c):
		return any(s.selector is not None and s.selector[0] == "wild" for s in c.segs)

	# `[#N]` needs a pre-existing instance and its `#` would start a comment
	# on a binding line; a path with a literal newline cannot be written at
	# all. Both go to the trailing note instead of emitting a broken line.
	# A path deeper than a document may nest cannot be generated either: the
	# line would draw E016 on the way back in.
	def unwritable(c):
		return len(c.segs) > MAX_DEPTH or any((s.selector is not None and s.selector[0] == "idx") or s.star for s in c.segs) or "\n" in c.path

	# Live concrete paths materialize instances; decide which must-exist
	# wildcards get filled (their first-wildcard parent chain is a prefix of
	# some live path). Fixpoint: a fill can materialize another's parent.
	def names_of(segs):
		return [s.name for s in segs]

	live = [names_of(c.segs) for c in cons if not has_wild(c) and not unwritable(c) and must_exist(c)]
	fill = [False] * len(cons)
	while True:
		changed = False
		for i, c in enumerate(cons):
			if fill[i] or not has_wild(c) or unwritable(c) or not must_exist(c):
				continue
			k = next(j for j, s in enumerate(c.segs) if s.selector is not None and s.selector[0] == "wild")
			parent = names_of(c.segs[: k + 1])
			if any(len(p) >= len(parent) and p[: len(parent)] == parent for p in live):
				fill[i] = True
				live.append(names_of(c.segs))
				changed = True
		if not changed:
			break
	out = []
	wild = []
	first = True
	for i, c in enumerate(cons):
		tyname = c.ty if c.ty is not None else "any"
		if unwritable(c) or (has_wild(c) and not fill[i]):
			wild.append((c.path.replace("\n", "\\n"), tyname))
			continue
		if not first:
			out.append("\n")
		first = False
		if c.desc is not None:
			for line in c.desc.split("\n"):
				out.append("# " + line + "\n")
		# The annotation is a comment: a newline smuggled in via an allowed
		# string value must not break out of it.
		out.append("# " + _gen_annotation(c, tyname).replace("\n", "\\n") + "\n")
		# A filled wildcard emits in dotted form, targeting the first (the
		# materialized) instance. Rebuilt from the parsed segments, not by
		# cutting text out of the path: the same path can be written several
		# ways, and only the segments say what it means.
		path = _gen_path_text(c.segs) if fill[i] else c.path
		prefix = "" if must_exist(c) else "#"
		if c.default_text is not None:
			out.append(f"{prefix}{path}: {_gen_default_text(c.default_text)}\n")
		else:
			out.append(f"{prefix}{path}:\n")
	# Cycle-cut mounts last: their "type" column names the fragment that
	# belongs at the path.
	wild.extend(cuts)
	if wild:
		if not first:
			out.append("\n")
		out.append("# Paths needing an instance name (not generated):\n")
		for path, tyname in wild:
			out.append(f"#   {path}   {tyname}\n")
	text = "".join(out)
	if not no_banner:
		if text:
			text += "\n"
		text += _GEN_BANNER
	return text, []


# Ceiling on how many fields one schema may expand to. Fragments that mount
# each other at more than one path multiply, so a short schema can otherwise
# ask for more output than the machine can hold; past this the generator
# reports a schema fault rather than running until something breaks.
_GEN_MAX_FIELDS = 10000

# Footer telling whoever opens the generated file what the format is and where
# its spec lives. It is output, so every binding emits these bytes exactly; the
# Legal line names SHCL as its subject so it cannot be read as a claim over the
# config it sits in.
_GEN_BANNER = (
	"#\n"
	'# This config file format is SHCL.\n'
	'# "Simple Hierarchical Config Language"\n'
	"#    Home     https://github.com/jim-collier/shcl\n"
	"#    Syntax   https://github.com/jim-collier/shcl/blob/main/project/spec.md\n"
	"#    Legal    SHCL is Copyright © 2026 Jim Collier. License: MIT. No warranty.\n"
	"#\n"
)


def _gen_path_text(segs):
	"""Render parsed segments back as a dotted path, dropping wildcard selectors
	(a generated line targets the one instance it materializes) and quoting a
	name that needs it, so the result is a path the scanner reads back the same."""
	out = []
	for i, s in enumerate(segs):
		if i > 0:
			out.append(".")
		if s.star:
			out.append("*")
		else:
			out.append(_emit_name(s.name))
		if s.selector is not None:
			if s.selector[0] == "val":
				out.append(f"[{_quote_text(s.selector[1]) if s.selector[2] else s.selector[1]}]")
			elif s.selector[0] == "idx":
				out.append(f"[#{s.selector[1]}]")
			# a wildcard selector is dropped
	return "".join(out)


def _expand_mounts(sdef):
	"""Inline every fragment mount into a flat constraint list, depth-first in
	schema order, each field's path and segments prefixed by its mount's. A
	mount whose fragment is already expanding (a cycle) stops there and is
	returned as (path, fragment name) for the trailing not-generated block."""
	out: list = []
	cuts = []
	stack: list = []

	def go(lst, at):
		for c in lst:
			cc = c.clone()
			if at is not None:
				p, s = at
				cc.path = f"{p}.{c.path}"
				cc.segs = list(s) + list(c.segs)
			path = cc.path
			segs = cc.segs
			if len(out) >= _GEN_MAX_FIELDS:
				return
			out.append(cc)
			if c.inherits is not None:
				# A chain long enough to outrun the stack, or a mount that
				# re-enters, stops here and is noted instead of expanded.
				if c.inherits in stack or len(stack) >= MAX_DEPTH:
					cuts.append((path.replace("\n", "\\n"), c.inherits))
				else:
					fcs = sdef.frags.get(c.inherits)
					if fcs is not None:
						stack.append(c.inherits)
						go(fcs, (path, segs))
						stack.pop()

	go(sdef.cons, None)
	return out, cuts


def _edit_distance(a, b):
	# Two-row Levenshtein; powers the "did you mean" prose (never the code).
	prev = list(range(len(b) + 1))
	cur = [0] * (len(b) + 1)
	for i in range(1, len(a) + 1):
		cur[0] = i
		for j in range(1, len(b) + 1):
			cost = 0 if a[i - 1] == b[j - 1] else 1
			cur[j] = min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost)
		prev, cur = cur, prev
	return prev[len(b)]


def _chain_push(chain, name):
	"""Append a segment to a chain key. Chain keys join segments
	length-prefixed (`<len>:<name>`), not with a bare NUL: NUL is legal in a
	quoted name, so a single field named "x\\0y" would impersonate the
	two-segment path x.y. Same injectivity reasoning as the merge key's cell
	encoding - and like it, the length unit is each binding's native one
	(code points here), because only injectivity matters."""
	return chain + str(len(name)) + ":" + name


def _chain_parts(chain):
	"""Decode a chain key back into its segments. Total: bails at the first
	shape the encoder can't have produced."""
	parts = []
	i = 0
	while i < len(chain):
		n = 0
		while i < len(chain) and "0" <= chain[i] <= "9":
			n = n * 10 + (ord(chain[i]) - 48)
			i += 1
		if i >= len(chain) or chain[i] != ":" or i + 1 + n > len(chain):
			break
		i += 1
		parts.append(chain[i:i + n])
		i += n
	return parts


def _star_legal(pats, chain):
	"""Element-wise chain match against the star-bearing schema paths: a `*`
	segment matches any one name, and every prefix of a path is legal."""
	if not pats:
		return False
	parts = _chain_parts(chain)
	return any(
		len(p) >= len(parts) and all(p[i].star or p[i].name == seg for i, seg in enumerate(parts))
		for p in pats
	)


def _chain_legal(cons, frags, chain):
	"""Chain legality through fragment mounts: the general matcher - element-
	wise like _star_legal (stars wild, prefixes legal), and when a mount's whole
	path matched with chain left over, the remainder is retried against the
	mounted fragment's fields. Terminates: every descent consumes >= 1 part."""
	parts = _chain_parts(chain)
	return _chain_parts_legal(cons, frags, parts)


def _chain_parts_legal(cons, frags, parts):
	for c in cons:
		n = len(c.segs)
		k = min(len(parts), n)
		if all(c.segs[i].star or c.segs[i].name == parts[i] for i in range(k)):
			if len(parts) <= n:
				return True
			if c.inherits is not None:
				fcs = frags.get(c.inherits)
				if fcs is not None and _chain_parts_legal(fcs, frags, parts[n:]):
					return True
	return False


def _v_suggest(siblings, parent_chain, name):
	"""Closest legal sibling name (same parent chain, schema order, edit
	distance <= 2) as "; did you mean 'x'?" - or nothing. Prose only, never
	contract. The sibling lists are prebuilt once per validate."""
	best = None
	for s in siblings.get(parent_chain, ()):
		dist = _edit_distance(name, s)
		if dist <= 2 and (best is None or dist < best[0]):
			best = (dist, s)
	if best is None:
		return ""
	return f"; did you mean '{best[1]}'?"
