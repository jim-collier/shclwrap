#!/usr/bin/env python3
## Purpose: Test dece1. The fuzz corpus keeps each case doc's bytes, CRLF
##	included (2026102410113346). Test dedph, the class Judge. A short run is
##	too short to judge the generator, and a long one is still judged
##	(2026102508080859).
## Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
## SPDX-License-Identifier: MIT

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

here = Path(__file__).resolve().parent
sys.path.insert(0, str(here))

import fuzz  # noqa: E402

CASES = here.parent / "tests" / "cases"


class LoadCorpus(unittest.TestCase):
	def test_keeps_bytes(self) -> None:
		paths = sorted(CASES.glob("*/doc*.shcl"))
		want = [p.read_bytes() for p in paths]
		# No CRLF doc left in the cases would make this pass on nothing.
		self.assertTrue(any(b"\r\n" in w for w in want), "no case doc has CRLF")
		got = [doc.encode("utf-8") for doc in fuzz.load_corpus(CASES)]
		self.assertEqual(len(got), len(want))
		for path, g, w in zip(paths, got, want, strict=True):
			self.assertEqual(g, w, f"{path.parent.name}/{path.name}")


# Stands in for every driver. It prints FAKE_READS reads per case, FAKE_GOOD of
# them with a value, and exits FAKE_EXIT. 2 keeps the output checks from running.
FAKE_DRIVER = """import os, sys
sys.stdin.read()
good = int(os.environ["FAKE_GOOD"])
print("\\n".join(["Good\\tx"] * good + ["NotFound\\tx"] * (int(os.environ["FAKE_READS"]) - good)))
sys.exit(int(os.environ["FAKE_EXIT"]))
"""


class Judge(unittest.TestCase):
	def fuzz(self, cases: int, reads: int, good: int, code: int = 2) -> subprocess.CompletedProcess[str]:
		with tempfile.TemporaryDirectory() as tmp:
			drive = Path(tmp) / "drive"
			drive.write_text(f"#!{sys.executable}\n{FAKE_DRIVER}", encoding="utf-8")
			drive.chmod(0o755)
			env = {**os.environ, "FAKE_READS": str(reads), "FAKE_GOOD": str(good), "FAKE_EXIT": str(code)}
			args = ["--go", drive, "--rust", drive, "--seed", 1, "--count", cases, "--seconds", 600, "--cases", CASES]
			args += ["--work", f"{tmp}/work", "--out", f"{tmp}/out"]
			cmd = [sys.executable, str(here / "fuzz.py"), *map(str, args)]
			return subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", env=env, check=False)

	def test_short_run_passes(self) -> None:
		# The busy-box run from the bug: 8 cases and 44 reads.
		p = self.fuzz(8, 5, 1)
		self.assertEqual(p.returncode, 0, p.stderr)
		self.assertIn("too few to judge the generator", p.stdout)

	def test_short_run_still_fails_a_crash(self) -> None:
		p = self.fuzz(8, 5, 1, code=3)
		self.assertEqual(p.returncode, 1)
		self.assertIn("crashed", p.stderr)

	def test_few_reads_fail(self) -> None:
		p = self.fuzz(fuzz.JUDGE_CASES, 1, 1)
		self.assertEqual(p.returncode, 1)
		self.assertIn("drifted", p.stderr)

	def test_few_values_fail(self) -> None:
		p = self.fuzz(fuzz.JUDGE_CASES, 20, 1)
		self.assertEqual(p.returncode, 1)
		self.assertIn("drifted", p.stderr)

	def test_long_run_passes(self) -> None:
		p = self.fuzz(fuzz.JUDGE_CASES, 5, 1)
		self.assertEqual(p.returncode, 0, p.stderr)
		self.assertNotIn("too few to judge", p.stdout)


if __name__ == "__main__":
	unittest.main()
