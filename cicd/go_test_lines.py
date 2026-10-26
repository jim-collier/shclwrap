#!/usr/bin/env python3
## Purpose: Reads `go test -json` on stdin and prints a PASS, FAIL or SKIP line
##	per shared case, by the case's test ID (2026102507142902), and per test
##	function, by the ID on its comment (2026102507142905). A failed test's
##	output and each package's ok line come through as go test prints them.
##	Exits 1 when any test or build failed, or a test has no ID.
## Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
## SPDX-License-Identifier: MIT

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

from c_cases import case_id
from test_ids import RUNNERS, ids

CASE_TEST = re.compile(r"TestCases/([^/]+)/v\d+")
SKIP_WHY = re.compile(r"^\s+\S+_test\.go:\d+: (.+)$")


def main() -> int:
	ap = argparse.ArgumentParser(description="Turns go test -json into test lines.")
	ap.add_argument("--cases", required=True, type=Path, help="the shared tests/cases dir, for the case IDs")
	args = ap.parse_args()

	said: dict[tuple[str, str], list[str]] = {}
	go_ids = ids("Go")
	failed = False
	for raw in sys.stdin:
		try:
			ev = json.loads(raw)
		except json.JSONDecodeError:
			print(raw, end="", flush=True)
			continue
		action, test = ev.get("Action"), ev.get("Test", "")
		key = (ev.get("Package") or ev.get("ImportPath", ""), test)
		if action in ("output", "build-output"):
			said.setdefault(key, []).append(ev.get("Output", ""))
			continue
		if action not in ("pass", "fail", "skip", "build-fail"):
			continue
		lines = said.pop(key, [])
		m = CASE_TEST.fullmatch(test)
		name = f"case {m.group(1)}" if m else test
		## A subtest rolls up into its function's line.
		if m:
			tid = case_id(args.cases / m.group(1))
			if not tid:
				print(f"  {m.group(1)}: no '# Test ID:' first line in ops.txt", file=sys.stderr, flush=True)
		elif test and "/" not in test and test not in RUNNERS:
			tid = go_ids.get(test)
			if not tid:
				print(f"  {test}: no '// Test ID:' line just above it", file=sys.stderr, flush=True)
		else:
			name = ""
		if name and not tid:
			failed = True
		elif name:
			why = next((w.group(1) for w in map(SKIP_WHY.match, lines) if w), "") if action == "skip" else ""
			print(f"{action.upper()} {tid} Go {name}{', ' + why if why else ''}", flush=True)
		if action in ("fail", "build-fail"):
			failed = True
			sys.stderr.write("".join(lines))
			sys.stderr.flush()
		elif not test:
			print("".join(line for line in lines if line.startswith("ok ")), end="", flush=True)
	return 1 if failed else 0


if __name__ == "__main__":
	sys.exit(main())
