#!/usr/bin/env python3
# Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
# SPDX-License-Identifier: MIT

"""Runs an ops script against a document, for the tests that compare
wrapper languages.

	python3 shclw_drive.py VERSION DOCFILE [DIR] < SCRIPT

VERSION is 3. DIR is where the script's check and write ops find and put files.
"""

from __future__ import annotations

import sys


def main(argv: list[str]) -> int:
	if not 3 <= len(argv) <= 4:
		print("usage: shclw_drive.py VERSION DOCFILE [DIR] < SCRIPT", file=sys.stderr)
		return 2
	if argv[1] != "3":
		print(f"shclw_drive: no such version: {argv[1]}", file=sys.stderr)
		return 2
	import shclw

	try:
		doc = shclw.read_text(argv[2])
		script = sys.stdin.buffer.read().decode("utf-8")
	except (shclw.Error, UnicodeDecodeError) as e:
		print(f"shclw_drive: {e}", file=sys.stderr)
		return 2
	out = shclw.run(doc, script, argv[3] if len(argv) == 4 else "")
	sys.stdout.buffer.write(out.encode("utf-8", "surrogateescape"))
	return 0


if __name__ == "__main__":
	sys.exit(main(sys.argv))
