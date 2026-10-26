#!/usr/bin/env python3
## Purpose: Finds the Go, Rust, Python and C tests and the test ID each one
##	carries (2026102507142905). Go, Rust and Python keep it on a
##	`Test ID: <id>` comment just above the test, C in api_test.c's table.
## Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
## SPDX-License-Identifier: MIT

from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path

from c_cases import case_id

ROOT = Path(__file__).resolve().parent.parent
ID = r"[0-9a-hjkmnp-tv-z]{5}"
ID_COMMENT = re.compile(rf"^\s*(?://|#) Test ID: ({ID})\s*$")
C_ROW = re.compile(rf'^\s*\{{"({ID})", "(test_\w+)", \2\}},')
## The shared case runners have no ID of their own, since each case has one.
RUNNERS = {"TestCases"}


@dataclass(frozen=True)
class Test:
	lang: str
	name: str
	tid: str | None
	where: str


def sources() -> dict[str, list[Path]]:
	src = ROOT / "source"
	return {
		"Go": sorted((src / "go").rglob("*_test.go")),
		"Rust": sorted([*(src / "rust" / "src").rglob("*.rs"), *(src / "rust" / "tests").glob("*.rs")]),
		"Python": sorted((src / "python" / "tests").glob("test*.py")),
		"C": [src / "c" / "tests" / "api_test.c"],
	}


def scan(lang: str, path: Path) -> list[Test]:
	lines = path.read_text(encoding="utf-8").splitlines()
	if lang == "C":
		return [
			Test(lang, m.group(2), m.group(1), f"{path.name}:{n}")
			for n, line in enumerate(lines, 1)
			if (m := C_ROW.match(line))
		]
	found: list[Test] = []
	pending: str | None = None
	is_test = False
	cls = ""
	for n, line in enumerate(lines, 1):
		if m := ID_COMMENT.match(line):
			pending = m.group(1)
			continue
		bare = line.strip()
		name = None
		if lang == "Go":
			if m := re.match(r"func ((?:Test|Fuzz)\w+)\(", line):
				name = m.group(1)
		elif lang == "Rust":
			if bare == "#[test]":
				is_test = True
			elif is_test and (m := re.match(r"(?:pub )?fn (\w+)\(", bare)):
				name, is_test = m.group(1), False
		elif m := re.match(r"class (\w+)\(", line):
			cls = m.group(1)
		elif cls and (m := re.match(r"\s+def (test_\w+)\(", line)):
			name = f"{cls}.{m.group(1)}"
		if name:
			if name not in RUNNERS:
				found.append(Test(lang, name, pending, f"{path.name}:{n}"))
			pending = None
		elif bare and not bare.startswith(("//", "#", "@")):
			pending = None
	return found


def all_tests() -> list[Test]:
	return [t for lang, paths in sources().items() for p in paths for t in scan(lang, p)]


def ids(lang: str) -> dict[str, str | None]:
	return {t.name: t.tid for p in sources()[lang] for t in scan(lang, p)}


## For cicd.bash --list: `tests LANG...` or `cases LANG...`, an ID and a name a line.
def main(argv: list[str]) -> int:
	langs = {"go": "Go", "rust": "Rust", "python": "Python", "c": "C"}
	if len(argv) < 2 or argv[0] not in ("tests", "cases") or not set(argv[1:]) <= langs.keys():
		print("usage: test_ids.py tests|cases go|rust|python|c...", file=sys.stderr)
		return 2
	picked = [langs[a] for a in langs if a in argv[1:]]
	if argv[0] == "tests":
		for t in all_tests():
			if t.lang in picked:
				print(f"{t.tid or '-----'} {t.lang} {t.name}")
		return 0
	for case in sorted(p for p in (ROOT / "tests" / "cases").iterdir() if p.is_dir()):
		print(f"{case_id(case) or '-----'} case {case.name} ({', '.join(picked)})")
	return 0


if __name__ == "__main__":
	sys.exit(main(sys.argv[1:]))
