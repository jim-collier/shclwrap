#!/usr/bin/env python3
## Purpose: Holds shcl's analyzer and CodeQL findings to the known list for its
##	pin, matched exactly, so a pin bump that adds one fails, and one that loses
##	one asks for the list to be pruned. The lists are source/c/shcl-known/.
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
	ls = sub.add_parser("list", help="print a report's findings as known-list lines")
	ls.add_argument("tool", choices=["gcc", "clang"])
	ls.add_argument("report", type=Path)
	args = ap.parse_args()

	if args.mode == "list":
		for (tool, where, check), n in sorted(analyzer_findings(args.tool, args.report).items()):
			for _ in range(n):
				print(f"{tool}\t{where}\t{check}")
		return 0
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
