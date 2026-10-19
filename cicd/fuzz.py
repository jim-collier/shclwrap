#!/usr/bin/env python3
## Purpose: Fuzzes the wrappers against each other. Each case is a document
##	plus an ops script, made from one seed, run through the Go, Rust, and
##	C drivers with the same shcl version. The outputs must match Go's byte
##	for byte, and a value that was put must read back the same.
## Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
## SPDX-License-Identifier: MIT

from __future__ import annotations

import argparse
import contextlib
import random
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass, field
from decimal import Decimal
from itertools import pairwise
from pathlib import Path

VERSIONS = (3,)
# A document is written in one of these formats. The old ones are what 1.2
# and 2.0 programs wrote, for the upgrade.
FORMATS = (1, 2, 3, 3)
NAMES = ("a", "b", "port", "name", "Server", "host", "tags", "x-y", "n_1", "list", "on")
WORDS = ("alpha", "beta", "two words", "café", "x#y", "a # b", "a, b", "'q'", '"dq"', "C:\\temp", "", " pad ")
TYPES = ("int", "float", "bool", "str", "strs", "ints", "floats", "raw")
TITLE = "This config file format is SHCL."
# The info block's spellings over time, 2.x's first. {n} is a format.
INFO_BLOCKS = (
	("#", "", "jim-collier", "main", ""),
	("##", "", "jim-collier", "main", ""),
	("##", "", "jim-collier", "main", " [ID: x]"),
	("##", "{n}", "jim-collier", "main", " [ID: x]"),
	("##", "{n}", "jim-collier", "v3.0.0", " [ID: x]"),
	("##", "{n}", "yottacore", "v3.0.0", " [ID: x]"),
	("##", "{n}", "yottacore", "v3.0.0-beta1", " [ID: x]"),
)


# shcl 3.0 stamps the time on a line it comments out to make room for a set,
# so 2 drivers a second apart print it differently.
NOTE_STAMP = re.compile(r"(## commented out by shcl when setting .*?, )\d{4}-\d\d-\d\d \d\d:\d\d:\d\d[^:\n]*(?=: )")


def unstamped(out: str) -> str:
	return NOTE_STAMP.sub(r"\1<time>", out)


@dataclass
class Case:
	seed: int
	version: int
	doc: str
	script: str
	puts: list[tuple[int, str, str, list[str]]] = field(default_factory=list)
	# Each edit's first count probe, how many there are, and the edit's index.
	edits: list[tuple[int, int, int]] = field(default_factory=list)
	# The doc may hold the block's title as a raw body's text, or as a
	# mutation left it, so a saved text can't be checked for one title.
	stray_titles: bool = False


def gen_scalar(rng: random.Random) -> str:
	kind = rng.randrange(6)
	if kind == 0:
		return str(rng.choice((0, 1, -7, 8080, 2**31, -(2**40), 2**63 - 1)))
	if kind == 1:
		return rng.choice(("0.5", "-2.25", "1e3", "3.14159", "1e-7", "100.0", "1e400", "-1e400", "nan", "inf"))
	if kind == 2:
		return rng.choice(("true", "false", "yes", "no", "on", "off"))
	if kind == 3:
		return rng.choice(WORDS)
	if kind == 4:
		return ""
	return rng.choice(NAMES)


def gen_array(rng: random.Random, fmt: int) -> str:
	items = [rng.choice(("1", "2", "red", "0.5", "x y")) for _ in range(rng.randrange(1, 4))]
	return ", ".join(items) if fmt < 3 else "[" + ", ".join(items) + "]"


def gen_block(rng: random.Random, fmt: int, depth: int, out: list[str]) -> None:
	for _ in range(rng.randrange(1, 5)):
		name = rng.choice(NAMES)
		if rng.random() < 0.1:
			name = f"{name}.{rng.choice(NAMES)}"
		pad = "\t" * depth
		roll = rng.random()
		if roll < 0.08:
			out.append(f"{pad}# {rng.choice(WORDS)}")
		elif roll < 0.12:
			out.append("")
		elif roll < 0.35 and depth < 3:
			val = rng.choice(("", " " + rng.choice(NAMES)))
			out.append(f"{pad}{name}:{val}")
			gen_block(rng, fmt, depth + 1, out)
		elif roll < 0.5:
			out.append(f"{pad}{name}: {gen_array(rng, fmt)}")
		else:
			out.append(f"{pad}{name}: {gen_scalar(rng)}".rstrip())


def gen_info(rng: random.Random, fmt: int) -> list[str]:
	mark, has_format, home, tag, legal_id = rng.choice(INFO_BLOCKS)
	n = fmt if rng.random() < 0.8 else rng.choice(FORMATS)
	lines = [mark, f"{mark} {TITLE}", f'{mark} "Simple Hierarchical Config Language"']
	if has_format:
		lines.append(f"{mark}    Format   {n}")
	lines += [
		f"{mark}    Home     https://github.com/{home}/shcl",
		f"{mark}    Syntax   https://github.com/{home}/shcl/blob/{tag}/project/spec.md",
		f"{mark}    Legal    SHCL is Copyright © 2026 Jim Collier{legal_id}. License: MIT. No warranty.",
		mark,
	]
	if rng.random() < 0.1:
		del lines[-1]
	return lines


def gen_doc(rng: random.Random, fmt: int) -> tuple[str, bool]:
	lines: list[str] = []
	gen_block(rng, fmt, 0, lines)
	if fmt >= 3 and rng.random() < 0.2:
		lines.append("##    Format   3")
	stray = False
	if rng.random() < 0.1:
		# A raw body holding the block's text is the body's, not a block.
		lines += ["tmpl:", "\t~~~", "##", f"## {TITLE}", f"##    Format   {fmt}", "##", "\t~~~"]
		stray = True
	if rng.random() < 0.3:
		at = rng.choice((0, len(lines), rng.randrange(len(lines) + 1)))
		lines[at:at] = gen_info(rng, fmt)
	return "\n".join(lines) + ("\n" if rng.random() < 0.9 else ""), stray


def gen_parts(rng: random.Random) -> list[str]:
	parts = [rng.choice(NAMES) for _ in range(rng.randrange(1, 4))]
	if len(parts) > 1 and rng.random() < 0.1:
		parts[rng.randrange(1, len(parts))] = "*"
	return parts


def gen_path(rng: random.Random) -> str:
	return ".".join(gen_parts(rng))


def add_edit(case: Case, ops: list[str], parts: list[str], op: str) -> None:
	"""Counts each step of the path, then makes the edit, for check_edits."""
	first = len(ops)
	ops.extend(f"count\t{'.'.join(parts[:k])}" for k in range(1, len(parts) + 1))
	ops.append(op)
	case.edits.append((first, len(parts), len(ops) - 1))


def esc_field(s: str) -> str:
	return s.replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n").replace("\r", "\\r")


def gen_value(rng: random.Random, typ: str) -> list[str]:
	if typ == "int":
		return [str(rng.choice((0, 42, -1, 2**53, -(2**63))))]
	if typ == "float":
		return [rng.choice(("0.5", "-1.25", "1e20", "3", "0.001", "123456.789", "1e400", "inf", "-inf", "nan"))]
	if typ == "bool":
		return [rng.choice(("true", "false"))]
	if typ == "str":
		return [rng.choice(("hello", "two words", "tab\there", "back\\slash", "café →", "x#y", "a, b", "["))]
	if typ == "strs":
		return [rng.choice(("a", "b c", "d,e", "")) for _ in range(rng.randrange(0, 4))]
	if typ == "ints":
		return [str(rng.randrange(-100, 100)) for _ in range(rng.randrange(0, 4))]
	return [rng.choice(("0.5", "2", "-3.75", "inf")) for _ in range(rng.randrange(0, 4))]


def gen_case(seed: int, corpus: list[str]) -> Case:
	rng = random.Random(seed)
	version = rng.choice(VERSIONS)
	if corpus and rng.random() < 0.25:
		doc, stray = mutate(rng, rng.choice(corpus)), True
	else:
		doc, stray = gen_doc(rng, rng.choice(FORMATS))
	case = Case(seed=seed, version=version, doc=doc, script="", stray_titles=stray)
	ops: list[str] = []
	# A quarter work on what Open made of the file, which an old one upgrades,
	# and end with 2 saves over it: the first backs it up, the second doesn't.
	opened = rng.random() < 0.25
	if opened:
		ops.append("open\tdoc.shcl")
	for _ in range(rng.randrange(3, 20)):
		roll = rng.random()
		if roll < 0.05:
			# Twice, for check_info: the second changes nothing.
			ops += ["info\ton", "text", "info\ton", "text"]
		elif roll < 0.35:
			ops.append(f"get\t{rng.choice(TYPES)}\t{gen_path(rng)}")
		elif roll < 0.47:
			typ = rng.choice(TYPES[:-1])
			parts = gen_parts(rng)
			path = ".".join(parts)
			vals = gen_value(rng, typ)
			add_edit(case, ops, parts, "\t".join(["put", typ, path, *map(esc_field, vals)]))
			case.puts.append((len(ops) - 1, typ, path, vals))
			ops.append(f"get\t{typ}\t{path}")
		elif roll < 0.55:
			# A one-name path finds a repeated key far more often.
			path = rng.choice((gen_path(rng), rng.choice(NAMES)))
			ops.append(f"lines\t{path}")
			ops.append(f"{rng.choice(('first', 'last'))}\t{rng.choice(TYPES)}\t{path}")
		elif roll < 0.6:
			parts = gen_parts(rng)
			op = rng.choice(("comment", "syntax"))
			add_edit(case, ops, parts, f"{op}\t{'.'.join(parts)}\t{rng.choice(WORDS)}")
		elif roll < 0.66:
			fallback = rng.choice((gen_value(rng, "int")[0], "0.5", "1e400", "nan"))
			ops.append(f"or\t{rng.choice(TYPES)}\t{gen_path(rng)}\t{fallback}")
		else:
			ops.append(
				rng.choice(
					(
						f"has\t{gen_path(rng)}",
						f"count\t{gen_path(rng)}",
						f"names\t{rng.choice(('', gen_path(rng)))}",
						f"each\t{gen_path(rng)}",
						f"eachget\tstr\t{gen_path(rng)}\t{rng.choice(NAMES)}",
						f"line\t{gen_path(rng)}",
						f"lines\t{gen_path(rng)}",
						f"remove\t{gen_path(rng)}",
						f"comment\t{gen_path(rng)}\t{rng.choice(WORDS)}",
						f"syntax\t{gen_path(rng)}\t{rng.choice(WORDS)}",
						f"info\t{rng.choice(('on', 'off'))}",
						"stamp",
						"text",
						"canonical",
						"rewrites",
						"diags",
						"errors",
						"format",
						f"upgrade\t{rng.choice(('', '2', '3'))}",
						f"parse\t{rng.choice(('loose', 'standard', 'strict'))}",
						"write\tout.shcl",
						# The driver's folder has the document as doc.shcl, so
						# an old one gets upgraded and backed up.
						f"open\tdoc.shcl{rng.choice(('', chr(9) + 'strict'))}",
						"upgraded",
						"write\tdoc.shcl",
						"files",
					)
				)
			)
	if opened:
		ops += ["upgraded", "rewrites", "write\tdoc.shcl", "upgraded", "write\tdoc.shcl", "files"]
	case.script = "\n".join(ops) + "\n"
	return case


def mutate(rng: random.Random, text: str) -> str:
	b = bytearray(text.encode())
	for _ in range(rng.randrange(1, 6)):
		if not b:
			break
		i = rng.randrange(len(b))
		roll = rng.random()
		if roll < 0.4:
			b[i] = rng.choice(b"\t\n :#,[]()\"'\\-.*xX0")
		elif roll < 0.7:
			del b[i]
		elif roll < 0.9:
			b.insert(i, rng.choice(b"\t\n :#,[]()"))
		else:
			b[i] ^= 1 << rng.randrange(8)
	return bytes(b).decode("utf-8", errors="replace")


def format_float(x: float) -> str:
	if x != x:
		return "nan"
	if x in (float("inf"), float("-inf")):
		return "inf" if x > 0 else "-inf"
	sign, digits, exponent = Decimal(repr(x)).normalize().as_tuple()
	ds = "".join(map(str, digits)) or "0"
	exp = len(ds) - 1 + int(exponent)
	neg = "-" if sign else ""
	if exp >= 17 or exp < -5:
		mant = ds[0] + ("." + ds[1:] if len(ds) > 1 else "")
		return f"{neg}{mant}e{exp}"
	if exp < 0:
		return f"{neg}0.{'0' * (-exp - 1)}{ds}"
	if len(ds) <= exp + 1:
		return f"{neg}{ds}{'0' * (exp + 1 - len(ds))}.0"
	return f"{neg}{ds[: exp + 1]}.{ds[exp + 1 :]}"


def esc_out(s: str) -> str:
	out = []
	for c in s:
		if c == "\\":
			out.append("\\\\")
		elif c == "\t":
			out.append("\\t")
		elif c == "\n":
			out.append("\\n")
		elif c == "\r":
			out.append("\\r")
		elif ord(c) < 0x20 or ord(c) == 0x7F:
			out.append(f"\\x{ord(c):02x}")
		else:
			out.append(c)
	return "".join(out)


def expected_values(typ: str, vals: list[str]) -> list[str]:
	if typ in ("float", "floats"):
		return [format_float(float(v)) for v in vals]
	if typ in ("str", "strs"):
		return [esc_out(v) for v in vals]
	return vals


def check_roundtrips(case: Case, output: str) -> list[str]:
	"""A put that printed ok must read back Good with the same value."""
	blocks = output.split("\n> ")
	problems = []
	for op_index, typ, path, vals in case.puts:
		if op_index + 1 >= len(blocks):
			continue
		put_lines = blocks[op_index].split("\n")
		get_lines = blocks[op_index + 1].split("\n")
		if len(put_lines) < 2 or put_lines[1] != "ok" or len(get_lines) < 2:
			continue
		fields = get_lines[1].split("\t")
		if fields[0] == "Empty" and not vals and typ in ("strs", "ints", "floats"):
			continue
		want = expected_values(typ, vals)
		got = fields[3:] if typ in ("strs", "ints", "floats") else fields[2:3]
		if typ in ("strs", "ints", "floats") and fields[2:3] != [str(len(vals))]:
			got = ["count " + "".join(fields[2:3])]
		if fields[0] != "Good" or got != want:
			problems.append(f"put {typ} {path!r} {vals!r} read back as {get_lines[1]!r}")
	return problems


FLOAT_READS = ("get\tfloat", "first\tfloat", "last\tfloat")


def check_finite(output: str) -> list[str]:
	"""A float read that says Good never gives infinity or NaN."""
	problems = []
	for block in output.split("\n> "):
		lines = block.removeprefix("> ").split("\n")
		if len(lines) < 2 or not lines[0].startswith(FLOAT_READS) or not lines[1].startswith("Good\t"):
			continue
		if {"inf", "-inf", "nan"} & set(lines[1].split("\t")[2:]):
			problems.append(f"{lines[0]!r} read as {lines[1]!r}")
	return problems


def check_edits(case: Case, output: str) -> list[str]:
	"""An edit that said ok had one instance at every step it found, as 3.0
	has it. A step with several makes it badpath, even an inner one."""
	blocks = output.split("\n> ")
	problems = []
	for first, steps, op_index in case.edits:
		if op_index >= len(blocks):
			continue
		counts = [(blocks[first + k].split("\n") + [""])[1] for k in range(steps)]
		said = (blocks[op_index].split("\n") + [""])[1]
		found = next((c for c in counts if c != "1"), "1")
		if said == "ok" and found.isdigit() and int(found) > 1:
			problems.append(f"{blocks[op_index].split(chr(10))[0]!r} said ok, with step counts {counts}")
	return problems


def check_picks(output: str) -> list[str]:
	"""A first or last read right after lines of the same path takes one
	instance, on the first or last of those lines."""
	problems = []
	blocks = [b.removeprefix("> ").split("\n") for b in output.split("\n> ")]
	for prev, cur in pairwise(blocks):
		if len(prev) < 2 or len(cur) < 2 or not prev[0].startswith("lines\t"):
			continue
		op = cur[0].split("\t")
		if op[0] not in ("first", "last") or op[2:3] != prev[0].split("\t")[1:2]:
			continue
		lines = prev[1].split("\t")[1:]
		status, line = (cur[1].split("\t") + [""])[:2]
		if not lines and "*" in op[2]:
			# The path comes back as it is, and a plain read of a wildcard
			# differs by version.
			continue
		if status == "Multiple":
			problems.append(f"{cur[0]!r} read as {cur[1]!r}")
		elif not lines and status not in ("NotFound", "BadPath"):
			problems.append(f"{cur[0]!r} read as {cur[1]!r}, with no instances")
		elif lines and line != (lines[0] if op[0] == "first" else lines[-1]):
			problems.append(f"{cur[0]!r} read as {cur[1]!r}, with lines {prev[1]!r}")
	return problems


def check_info(case: Case, output: str) -> list[str]:
	"""Text right after an info on that said ok ends with one block naming
	the version's format, and a second info on changes nothing."""
	problems = []
	blocks = [b.removeprefix("> ").split("\n") for b in output.split("\n> ")]
	texts: dict[int, list[str]] = {}
	for i, (op, cur) in enumerate(pairwise(blocks)):
		if op[0] != "info\ton" or op[1:2] != ["ok"] or cur[0] != "text":
			continue
		body = [line.removeprefix("| ").removesuffix("\\r") for line in cur[1:] if line.startswith("| ")]
		texts[i] = body
		tail = body[-8:]
		want = ["##", f"## {TITLE}", f"##    Format   {case.version}", "##"]
		if len(tail) < 8 or [tail[0], tail[1], tail[3], tail[7]] != want:
			problems.append(f"text after info on #{i} ends {tail!r}")
		titles = sum(line.strip(" \t").endswith(" " + TITLE) for line in body)
		if not case.stray_titles and titles != 1:
			problems.append(f"text after info on #{i} has {titles} info blocks")
		if i - 2 in texts and blocks[i - 1][0] == "text" and texts[i - 2] != body:
			problems.append(f"a second info on #{i} changed the text")
	return problems


def run_driver(cmd: list[str], version: int, doc: Path, work: Path, script: str) -> tuple[int, str, str]:
	if work.exists():
		shutil.rmtree(work)
	work.mkdir(parents=True)
	shutil.copyfile(doc, work / "doc.shcl")
	try:
		p = subprocess.run(
			[*cmd, str(version), str(doc), str(work)], input=script.encode(), capture_output=True, timeout=30
		)
	except subprocess.TimeoutExpired:
		return -1, "", "timeout"
	return p.returncode, p.stdout.decode(errors="replace"), p.stderr.decode(errors="replace")


def save_failure(out: Path, case: Case, why: str, runs: dict[str, tuple[int, str, str]]) -> None:
	d = out / f"seed-{case.seed}"
	d.mkdir(parents=True, exist_ok=True)
	(d / "doc.shcl").write_text(case.doc)
	(d / "ops.txt").write_text(case.script)
	(d / "why.txt").write_text(f"version {case.version}\n{why}\n")
	for name, (code, stdout, stderr) in runs.items():
		(d / f"{name}.out").write_text(f"exit {code}\n{stderr}\n---\n{stdout}")


def load_corpus(cases_dir: Path) -> list[str]:
	return [p.read_text() for p in sorted(cases_dir.glob("*/doc*.shcl"))]


def main() -> int:
	ap = argparse.ArgumentParser(description=__doc__)
	ap.add_argument("--go", required=True, help="Go driver binary")
	ap.add_argument("--rust", required=True, help="Rust driver binary")
	ap.add_argument("--c", default=None, help="C driver binary, with {v} where its shcl version goes")
	ap.add_argument("--seconds", type=float, default=30.0, help="wall-clock budget")
	ap.add_argument("--seed", type=int, default=None, help="first seed; default is from the clock")
	ap.add_argument("--count", type=int, default=0, help="stop after this many cases; 0 is no limit")
	ap.add_argument("--cases", type=Path, required=True, help="the shared tests/cases dir, used as a mutation corpus")
	ap.add_argument("--work", type=Path, required=True, help="scratch dir this run owns")
	ap.add_argument("--out", type=Path, required=True, help="where failing cases go")
	args = ap.parse_args()
	# C is built once per version, so its path has the version in it.
	drivers = {"go": args.go, "rust": args.rust}
	if args.c:
		drivers["c"] = args.c

	first = args.seed if args.seed is not None else int(time.time())
	corpus = load_corpus(args.cases)
	deadline = time.monotonic() + args.seconds
	ran = gets = good_gets = failures = 0
	seed = first
	doc_path = args.work / "doc.shcl"
	made_work = not args.work.exists()
	args.work.mkdir(parents=True, exist_ok=True)
	try:
		while time.monotonic() < deadline and (args.count == 0 or ran < args.count):
			case = gen_case(seed, corpus)
			doc_path.write_text(case.doc)
			runs = {
				name: run_driver(
					[cmd.replace("{v}", str(case.version))], case.version, doc_path, args.work / name, case.script
				)
				for name, cmd in drivers.items()
			}
			go = runs["go"]
			problems = []
			for name, run in runs.items():
				if run[0] not in (0, 2):
					problems.append(f"{name} crashed with exit {run[0]}")
				if name != "go" and run[0] != go[0]:
					problems.append(f"exit codes differ: go {go[0]}, {name} {run[0]}")
				if name != "go" and unstamped(run[1]) != unstamped(go[1]):
					problems.append(f"{name} output differs from go")
			if not problems and go[0] == 0:
				problems += check_roundtrips(case, go[1])
				problems += check_edits(case, go[1])
				problems += check_finite(go[1])
				problems += check_picks(go[1])
				problems += check_info(case, go[1])
			for line in go[1].split("\n"):
				if line.startswith(("Good\t", "Empty\t", "NotFound\t", "BadType\t", "Multiple\t", "BadPath\t")):
					gets += 1
					good_gets += line.startswith("Good\t")
			if problems:
				failures += 1
				save_failure(args.out, case, "\n".join(problems), runs)
				print(f"FAIL seed {seed} (shcl v{case.version}): {problems[0]}", file=sys.stderr)
			ran += 1
			seed += 1
	finally:
		# Only what this run made. --out may sit inside --work, so the folder goes only if empty.
		doc_path.unlink(missing_ok=True)
		for sub in drivers:
			shutil.rmtree(args.work / sub, ignore_errors=True)
		if made_work:
			with contextlib.suppress(OSError):
				args.work.rmdir()

	print(
		f"fuzz: {ran} cases from seed {first} through {', '.join(drivers)}, {failures} failed,"
		f" {good_gets} of {gets} reads found a value"
	)
	if ran == 0:
		print("fuzz: no cases ran", file=sys.stderr)
		return 1
	# A generator that stops making readable documents would pass forever.
	if gets < 50 or good_gets * 10 < gets:
		print("fuzz: too few reads found a value; the generator has drifted", file=sys.stderr)
		return 1
	return 1 if failures else 0


if __name__ == "__main__":
	sys.exit(main())
