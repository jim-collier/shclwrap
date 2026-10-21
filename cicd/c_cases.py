#!/usr/bin/env python3
## Purpose: Runs the shared cases through the C driver, or Python's, and
##	compares with the want files Go writes, op by op. Every op has to match.
## Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
## SPDX-License-Identifier: MIT

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def split_version(name: str) -> tuple[str, int]:
	"""doc.v3.shcl is ("doc.shcl", 3); a name with no version is version 0."""
	m = re.fullmatch(r"(.+)\.v(\d+)(\.[^.]+)", name)
	if m:
		return m.group(1) + m.group(3), int(m.group(2))
	return name, 0


def materialize(case: Path, into: Path, version: int) -> bool:
	"""Copies a case as `version` sees it: x.vK.ext stands in for x.ext from K up."""
	best: dict[str, tuple[int, Path]] = {}
	for f in case.iterdir():
		base, k = split_version(f.name)
		if base.startswith("want.") or k > version:
			continue
		if base in best and best[base][0] >= k:
			continue
		best[base] = (k, f)
	for base, (_, src) in best.items():
		shutil.copyfile(src, into / base)
	return "doc.shcl" in best


def lines_of(text: str) -> list[str]:
	"""Splits on newlines only, since a value can hold U+2028 and the like."""
	lines = text.split("\n")
	return lines[:-1] if lines and lines[-1] == "" else lines


def blocks(lines: list[str], echoes: list[str]) -> list[list[str]] | None:
	"""Each op's output lines, split at the echo lines the driver printed."""
	out: list[list[str]] = []
	at = 0
	for i, echo in enumerate(echoes):
		if at >= len(lines) or lines[at] != echo:
			return None
		at += 1
		nxt = echoes[i + 1] if i + 1 < len(echoes) else None
		start = at
		while at < len(lines) and lines[at] != nxt:
			at += 1
		out.append(lines[start:at])
	return out if at == len(lines) else None


def main() -> int:
	ap = argparse.ArgumentParser(description="Runs the shared cases through the C or Python driver.")
	ap.add_argument("--drive", required=True, nargs="+", help="the driver command, such as the C shclw-drive")
	ap.add_argument("--lang", default="C", help="the driver's language, for the summary")
	ap.add_argument("--version", type=int, required=True, help="the shcl version it was built for")
	ap.add_argument("--cases", required=True, type=Path)
	args = ap.parse_args()

	failures: list[str] = []
	ran = full = 0
	with tempfile.TemporaryDirectory(prefix="shclw-cases-") as tmp:
		for case in sorted(p for p in args.cases.iterdir() if p.is_dir()):
			label = f"{case.name}/v{args.version}"
			work = Path(tmp) / case.name
			work.mkdir()
			if not materialize(case, work, args.version):
				continue
			want_file = case / f"want.v{args.version}.txt"
			if not want_file.exists():
				failures.append(f"{label}: no want file; the Go tests write them")
				continue
			with open(work / "ops.txt", "rb") as script:
				proc = subprocess.run(
					[*args.drive, str(args.version), str(work / "doc.shcl"), str(work)],
					stdin=script,
					capture_output=True,
					timeout=60,
				)
			if proc.returncode != 0:
				failures.append(f"{label}: driver exit {proc.returncode}: {proc.stderr.decode(errors='replace')}")
				continue
			got = lines_of(proc.stdout.decode("utf-8"))
			want = lines_of(want_file.read_text(encoding="utf-8"))
			echoes = [line for line in got if line.startswith("> ")]
			got_blocks, want_blocks = blocks(got, echoes), blocks(want, echoes)
			if got_blocks is None or want_blocks is None:
				failures.append(f"{label}: the ops echoed differ from the want file")
				continue
			wrong = 0
			for echo, g, w in zip(echoes, got_blocks, want_blocks, strict=True):
				if g != w:
					wrong += 1
					failures.append(f"{label}: {echo!r}\n  want: {w}\n  got:  {g}")
			ran += len(echoes)
			full += wrong == 0

	print(f"{args.lang} cases: {full} whole, {ran} ops compared")
	if ran == 0:
		failures.append("no op was compared")
	for f in failures:
		print("FAIL " + f, file=sys.stderr)
	return 1 if failures else 0


if __name__ == "__main__":
	sys.exit(main())
