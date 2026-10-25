#!/usr/bin/env python3
## Purpose: Test dece1. The fuzz corpus keeps each case doc's bytes, CRLF
##	included (2026102410113346).
## Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
## SPDX-License-Identifier: MIT

from __future__ import annotations

import sys
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


if __name__ == "__main__":
	unittest.main()
