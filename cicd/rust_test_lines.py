#!/usr/bin/env python3
## Purpose: Reads `cargo test -- --format pretty` on stdin and prints a PASS,
##	FAIL or SKIP line per test function, by the ID on its comment
##	(2026102507142905). The shared case runner's own lines and a failure's
##	report come through as they are. Exits 1 when a test has no ID.
## Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
## SPDX-License-Identifier: MIT

from __future__ import annotations

import re
import sys

from test_ids import ids

TEST_LINE = re.compile(r"test (\S+) \.\.\. (ok|FAILED|ignored)(?:, (.+))?")
STATUS = {"ok": "PASS", "FAILED": "FAIL", "ignored": "SKIP"}


def main() -> int:
	rust_ids = ids("Rust")
	no_id = False
	for raw in sys.stdin:
		line = raw.rstrip("\n")
		m = TEST_LINE.fullmatch(line)
		if not m:
			if line.strip() and not re.fullmatch(r"running \d+ tests?", line):
				print(line, flush=True)
			continue
		## A unit test prints with its module path.
		name = m.group(1).rsplit("::", 1)[-1]
		tid = rust_ids.get(name)
		if not tid:
			print(f"  {m.group(1)}: no '// Test ID:' line just above it", file=sys.stderr, flush=True)
			no_id = True
			continue
		why = f", {m.group(3)}" if m.group(3) else ""
		print(f"{STATUS[m.group(2)]} {tid} Rust {name}{why}", flush=True)
	return 1 if no_id else 0


if __name__ == "__main__":
	sys.exit(main())
