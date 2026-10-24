#!/usr/bin/env python3
## Purpose: Holds shcl's analyzer, CodeQL and CTU findings to the known list
##	for its pin, matched exactly, so a pin bump that adds one fails, and one that
##	loses one asks for the list to be pruned. The lists are source/c/shcl-known/.
## Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
## SPDX-License-Identifier: MIT

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path

Finding = tuple[str, str, str]

## gcc puts its check last, after a CWE tag; clang has only the check.
WARNING = re.compile(r"^(?:\S*/)?([^/\s:]+):(\d+):(\d+): warning: .*\[([^\]\s]+)\]\s*$")


def read_known(path: Path) -> Counter[Finding]:
	if not path.is_file():
		sys.exit(f"no known list for this shcl pin: {path}")
	known: Counter[Finding] = Counter()
	for raw in path.read_text(encoding="utf-8").splitlines():
		line = raw.split("#", 1)[0].strip()
		if not line:
			continue
		fields = line.split()
		if len(fields) != 3:
			sys.exit(f"{path}: want tool, place, check: {raw}")
		known[(fields[0], fields[1], fields[2])] += 1
	return known


def analyzer_findings(tool: str, report: Path) -> Counter[Finding]:
	found: Counter[Finding] = Counter()
	for line in report.read_text(encoding="utf-8", errors="replace").splitlines():
		m = WARNING.match(line)
		if m:
			found[(tool, f"{m[1]}:{m[2]}:{m[3]}", m[4])] += 1
	return found


def codeql_findings(sarif: Path, rev: str) -> Counter[Finding]:
	prefix = f"cicd/cache/shcl-c/{rev}/"
	found: Counter[Finding] = Counter()
	for run in json.loads(sarif.read_text(encoding="utf-8"))["runs"]:
		for result in run.get("results") or []:
			if any(s.get("kind") == "inSource" for s in result.get("suppressions") or []):
				continue
			loc = (result.get("locations") or [{}])[0].get("physicalLocation", {})
			uri = loc.get("artifactLocation", {}).get("uri", "")
			if not uri.startswith(prefix):
				continue
			line = loc.get("region", {}).get("startLine", "-")
			found[("codeql", f"{Path(uri).name}:{line}", result["ruleId"])] += 1
	return found


## The CTU pass analyzes our files, so a finding is shcl's only by where it sits.
CTU_DIAG = re.compile(r"^(\S+?):(\d+):(\d+): (warning|error|fatal error): (.*?)(?: \[([^\]\s]+)\])?\s*$")
SOURCE_QUOTE = re.compile(r"^\s*\d*\s*\|")


def ctu_findings(logs: list[Path], revs: list[str]) -> tuple[dict[str, Counter[Finding]], list[str]]:
	found: dict[str, Counter[Finding]] = {rev: Counter() for rev in revs}
	ours: list[str] = []
	for log in logs:
		text = log.read_text(encoding="utf-8", errors="replace")
		diags = 0
		for line in text.splitlines():
			m = CTU_DIAG.match(line)
			if not m:
				## clang's own errors name no place. Quoted source lines start with "NNN |".
				if "error:" in line and not SOURCE_QUOTE.match(line):
					ours.append(f"{log.name}: {line}")
					diags += 1
				continue
			diags += 1
			rev = next((r for r in revs if f"/cicd/cache/shcl-c/{r}/" in m[1]), None)
			if rev is None or m[4] != "warning" or not m[6]:
				ours.append(f"{log.name}: {line}")
				continue
			found[rev][("ctu", f"{Path(m[1]).name}:{m[2]}:{m[3]}", m[6])] += 1
		## A crash or a stray message names no place, and is still a failed pass.
		if text.strip() and not diags:
			ours.append(f"{log.name}: output but no diagnostic; read the log")
	return found, ours


def compare(found: Counter[Finding], known: Counter[Finding], tools: set[str], list_path: Path) -> int:
	want = Counter({k: n for k, n in known.items() if k[0] in tools})
	new = found - want
	gone = want - found
	for (tool, where, check), n in sorted(new.items()):
		for _ in range(n):
			print(f"shcl finding not in {list_path}: {tool}\t{where}\t{check}", file=sys.stderr)
	for (tool, where, check), n in sorted(gone.items()):
		for _ in range(n):
			print(f"shcl finding gone, prune it from {list_path}: {tool}\t{where}\t{check}", file=sys.stderr)
	if new or gone:
		return 1
	print(f"shcl {'/'.join(sorted(tools))} findings match {list_path}: {sum(found.values())}")
	return 0


def main() -> int:
	ap = argparse.ArgumentParser()
	sub = ap.add_subparsers(dest="mode", required=True)
	a = sub.add_parser("analyzer", help="check a gcc or clang report")
	a.add_argument("known", type=Path)
	a.add_argument("tool", choices=["gcc", "clang"])
	a.add_argument("report", type=Path)
	c = sub.add_parser("codeql", help="check the shcl results in a SARIF for each pin")
	c.add_argument("known_dir", type=Path)
	c.add_argument("sarif", type=Path)
	c.add_argument("revs", nargs="+")
	t = sub.add_parser("ctu", help="check the CTU logs: none of ours, and shcl's as listed per pin")
	t.add_argument("known_dir", type=Path)
	t.add_argument("--rev", action="append", required=True)
	t.add_argument("logs", type=Path, nargs="+")
	ls = sub.add_parser("list", help="print a report's findings as known-list lines")
	ls.add_argument("tool", choices=["gcc", "clang", "ctu"])
	ls.add_argument("report", type=Path)
	ls.add_argument("--rev", action="append", help="with ctu, the pin to list")
	args = ap.parse_args()

	if args.mode == "list":
		if args.tool == "ctu":
			if not args.rev:
				ap.error("list ctu needs --rev")
			listed = ctu_findings([args.report], args.rev)[0][args.rev[0]]
		else:
			listed = analyzer_findings(args.tool, args.report)
		for (tool, where, check), n in sorted(listed.items()):
			for _ in range(n):
				print(f"{tool}\t{where}\t{check}")
		return 0
	if args.mode == "ctu":
		by_rev, ours = ctu_findings(args.logs, args.rev)
		for line in ours:
			print(f"CTU finding of ours, or a failed pass: {line}", file=sys.stderr)
		failed = 1 if ours else 0
		for rev in args.rev:
			known_path = args.known_dir / f"{rev}.txt"
			failed |= compare(by_rev[rev], read_known(known_path), {"ctu"}, known_path)
		return failed
	if args.mode == "analyzer":
		found = analyzer_findings(args.tool, args.report)
		return compare(found, read_known(args.known), {args.tool}, args.known)
	failed = 0
	for rev in args.revs:
		known_path = args.known_dir / f"{rev}.txt"
		failed |= compare(codeql_findings(args.sarif, rev), read_known(known_path), {"codeql"}, known_path)
	return failed


if __name__ == "__main__":
	sys.exit(main())
