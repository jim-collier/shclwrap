#!/usr/bin/env python3
## Purpose: Runs the Python tests in DIR, as `python3 -m unittest discover -q
##	-s DIR` would, and prints a PASS, FAIL or SKIP line per test function, by
##	the ID on its comment (2026102507142905). Exits 1 when a test failed or
##	has no ID.
## Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
## SPDX-License-Identifier: MIT

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_ids import scan  # noqa: E402

py_ids: dict[str, str | None] = {}
no_id: list[str] = []


class Lines(unittest.TextTestResult):
	def startTest(self, test: unittest.TestCase) -> None:
		super().startTest(test)
		self.status, self.why = "PASS", ""

	def stopTest(self, test: unittest.TestCase) -> None:
		super().stopTest(test)
		## test_module.Class.test_name
		name = ".".join(test.id().split(".")[-2:])
		tid = py_ids.get(name)
		if not tid:
			no_id.append(name)
			print(f"  {name}: no '# Test ID:' line just above it", file=sys.stderr, flush=True)
			return
		print(f"{self.status} {tid} Python {name}{', ' + self.why if self.why else ''}", flush=True)

	def addFailure(self, test, err) -> None:  # noqa: ANN001
		super().addFailure(test, err)
		self.status = "FAIL"

	def addError(self, test, err) -> None:  # noqa: ANN001
		super().addError(test, err)
		self.status = "FAIL"

	def addSubTest(self, test, subtest, err) -> None:  # noqa: ANN001
		super().addSubTest(test, subtest, err)
		if err is not None:
			self.status = "FAIL"

	def addUnexpectedSuccess(self, test) -> None:  # noqa: ANN001
		super().addUnexpectedSuccess(test)
		self.status = "FAIL"

	def addSkip(self, test: unittest.TestCase, reason: str) -> None:
		super().addSkip(test, reason)
		self.status, self.why = "SKIP", reason


def main() -> int:
	if len(sys.argv) != 2:
		print("usage: py_tests.py DIR", file=sys.stderr)
		return 2
	## As `python3 -m unittest` does, so the tests import from where it runs.
	sys.path.insert(0, str(Path.cwd()))
	py_ids.update((t.name, t.tid) for p in Path(sys.argv[1]).glob("test*.py") for t in scan("Python", p))
	suite = unittest.TestLoader().discover(sys.argv[1])
	result = unittest.TextTestRunner(stream=sys.stderr, verbosity=0, resultclass=Lines).run(suite)
	return 0 if result.wasSuccessful() and not no_id else 1


if __name__ == "__main__":
	sys.exit(main())
