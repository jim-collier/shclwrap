#!/usr/bin/env python3
## Purpose: Reads `go test -json` on stdin and prints a PASS, FAIL or SKIP line
##	per shared case, by the case's test ID (2026102507142902). A failed test's
##	output and each package's ok line come through as go test prints them.
##	Exits 1 when any test or build failed.
## Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
## SPDX-License-Identifier: MIT

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

from c_cases import case_id

CASE_TEST = re.compile(r"TestCases/([^/]+)/v\d+")
SKIP_WHY = re.compile(r"^\s+\S+_test\.go:\d+: (.+)$")


def main() -> int:
	ap = argparse.ArgumentParser(description="Turns go test -json into test lines.")
	ap.add_argument("--cases", required=True, type=Path, help="the shared tests/cases dir, for the case IDs")
	args = ap.parse_args()

	said: dict[tuple[str, str], list[str]] = {}
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
		tid = case_id(args.cases / m.group(1)) if m else None
		if m and not tid:
			print(f"  {m.group(1)}: no '# Test ID:' first line in ops.txt", file=sys.stderr, flush=True)
			failed = True
		elif m:
			why = next((w.group(1) for w in map(SKIP_WHY.match, lines) if w), "") if action == "skip" else ""
			print(f"{action.upper()} {tid} Go case {m.group(1)}{', ' + why if why else ''}", flush=True)
		if action in ("fail", "build-fail"):
			failed = True
			sys.stderr.write("".join(lines))
			sys.stderr.flush()
		elif not test:
			print("".join(line for line in lines if line.startswith("ok ")), end="", flush=True)
	return 1 if failed else 0


if __name__ == "__main__":
	sys.exit(main())
