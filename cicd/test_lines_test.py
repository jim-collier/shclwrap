#!/usr/bin/env python3
## Purpose: Test dee8t. Every shared case has its own test ID, and the case
##	and fuzz runs print one PASS, FAIL or SKIP line per test by it
##	(2026102507142902, 2026102507142907).
## Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
## SPDX-License-Identifier: MIT

from __future__ import annotations

import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

here = Path(__file__).resolve().parent
sys.path.insert(0, str(here))

import c_cases  # noqa: E402
import fuzz  # noqa: E402

CASES = here.parent / "tests" / "cases"
TEST_ID = re.compile(r"[0-9a-hjkmnp-tv-z]{5}")


def run(*cmd: str, stdin: str = "") -> subprocess.CompletedProcess[str]:
	return subprocess.run(cmd, input=stdin, capture_output=True, text=True, encoding="utf-8", check=False)


class CaseIds(unittest.TestCase):
	def test_each_case_has_its_own(self) -> None:
		seen: dict[str, str] = {}
		for case in sorted(p for p in CASES.iterdir() if p.is_dir()):
			tid = c_cases.case_id(case)
			self.assertIsNotNone(tid, f"{case.name}: no '# Test ID:' first line in ops.txt")
			self.assertNotIn(tid, seen, f"{case.name} and {seen.get(str(tid))} share {tid}")
			seen[str(tid)] = case.name
		self.assertGreater(len(seen), 50)
		taken = {tid for tid, _ in fuzz.TESTS.values()}
		bash = "\n".join(
			p.read_text(encoding="utf-8") for p in [*here.glob("*.bash"), *here.glob("utility/include/*.bash")]
		)
		taken |= {
			a or b
			for a, b in re.findall(r"(?:fTest(?: -q)?|fTest_Start|fTest_Skip) ([0-9a-z]{5}) |\[([0-9a-z]{5})\]=", bash)
		}
		self.assertFalse(taken & seen.keys(), "a case ID is also a cicd or fuzz test's")


class CaseLines(unittest.TestCase):
	def test_lines(self) -> None:
		with tempfile.TemporaryDirectory() as tmp:
			cases = Path(tmp)
			for name, ops in (
				("1-good", "# Test ID: aaaa1\nget\tstr\ta\n"),
				("2-bad", "# Test ID: aaaa2\nget\tstr\tb\n"),
			):
				(cases / name).mkdir()
				(cases / name / "ops.txt").write_text(ops, encoding="utf-8")
				(cases / name / "doc.shcl").write_text("a: x\n", encoding="utf-8")
				(cases / name / "want.v3.txt").write_text("> get\tstr\ta\nGood\t1\tx\n", encoding="utf-8")
			(cases / "3-no-doc").mkdir()
			(cases / "3-no-doc" / "ops.txt").write_text("# Test ID: aaaa3\n", encoding="utf-8")
			(cases / "4-no-id").mkdir()
			(cases / "4-no-id" / "ops.txt").write_text("get\tstr\ta\n", encoding="utf-8")
			# Echoes each op, then a fixed read, so 1-good matches and 2-bad doesn't.
			drive = cases / "drive.py"
			drive.write_text(
				"import sys\nfor op in sys.stdin.read().splitlines()[1:]: print('> ' + op); print('Good\\t1\\tx')\n",
				encoding="utf-8",
			)
			p = run(
				sys.executable,
				str(here / "c_cases.py"),
				"--lang",
				"Zig",
				"--version",
				"3",
				"--cases",
				tmp,
				"--drive",
				sys.executable,
				str(drive),
			)
		lines = [line for line in p.stdout.splitlines() if line[:5] in ("PASS ", "FAIL ", "SKIP ")]
		self.assertEqual(
			lines,
			[
				"PASS aaaa1 Zig case 1-good",
				"FAIL aaaa2 Zig case 2-bad",
				"SKIP aaaa3 Zig case 3-no-doc, no doc for shcl v3",
			],
		)
		self.assertIn("4-no-id/v3: no '# Test ID:' first line", p.stderr)
		self.assertEqual(p.returncode, 1)

	def test_go_json(self) -> None:
		events = [
			{"Action": "pass", "Package": "p", "Test": "TestCases/001-reads/v3"},
			{
				"Action": "output",
				"Package": "p",
				"Test": "TestCases/002-each/v3",
				"Output": "    cases_test.go:45: no doc for this version\n",
			},
			{"Action": "skip", "Package": "p", "Test": "TestCases/002-each/v3"},
			{
				"Action": "output",
				"Package": "p",
				"Test": "TestCases/003-put/v3",
				"Output": "    cases_test.go:62: output differs\n",
			},
			{"Action": "fail", "Package": "p", "Test": "TestCases/003-put/v3"},
			{"Action": "pass", "Package": "p", "Test": "TestOther"},
			{"Action": "output", "Package": "p", "Output": "ok  \tp\t0.1s\n"},
			{"Action": "fail", "Package": "p"},
		]
		p = run(
			sys.executable,
			str(here / "go_test_lines.py"),
			"--cases",
			str(CASES),
			stdin="".join(json.dumps(e) + "\n" for e in events),
		)
		ids = [c_cases.case_id(CASES / n) for n in ("001-reads", "002-each", "003-put")]
		self.assertEqual(
			p.stdout.splitlines(),
			[
				f"PASS {ids[0]} Go case 001-reads",
				f"SKIP {ids[1]} Go case 002-each, no doc for this version",
				f"FAIL {ids[2]} Go case 003-put",
			],
		)
		self.assertIn("output differs", p.stderr)
		self.assertEqual(p.returncode, 1)


class FuzzLines(unittest.TestCase):
	def fuzz(self, go: str, rust: str) -> list[str]:
		with tempfile.TemporaryDirectory() as tmp:
			args = [
				"--go",
				go,
				"--rust",
				rust,
				"--seed",
				"1",
				"--count",
				"2",
				"--cases",
				str(CASES),
				"--work",
				f"{tmp}/w",
				"--out",
				f"{tmp}/o",
			]
			p = run(sys.executable, str(here / "fuzz.py"), *args)
		return [line for line in p.stdout.splitlines() if line[:5] in ("PASS ", "FAIL ", "SKIP ")]

	def test_ids(self) -> None:
		ids = [tid for tid, _ in fuzz.TESTS.values()]
		self.assertEqual(len(set(ids)), len(ids))
		self.assertTrue(all(TEST_ID.fullmatch(tid) for tid in ids))

	def test_blames_the_driver(self) -> None:
		got = self.fuzz("/bin/true", "/bin/false")
		self.assertEqual([line[:10] for line in got], ["PASS ddzw1", "FAIL ddzw2", "SKIP de57g", "SKIP de8ed"])
		self.assertTrue(got[2].endswith(", no C driver"), got[2])
		# A Go crash is Go's, whatever the others did.
		self.assertEqual(self.fuzz("/bin/false", "/bin/false")[0][:10], "FAIL ddzw1")

	def test_skip(self) -> None:
		p = run(sys.executable, str(here / "fuzz.py"), "--skip=--no-fuzz")
		self.assertEqual(
			p.stdout.splitlines(), [f"SKIP {tid} {title}, --no-fuzz" for tid, title in fuzz.TESTS.values()]
		)
		self.assertEqual(p.returncode, 0)


if __name__ == "__main__":
	unittest.main()
