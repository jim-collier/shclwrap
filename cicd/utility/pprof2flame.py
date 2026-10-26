#!/usr/bin/env python3

##	Purpose: Turn a Go `go test -cpuprofile` profile into an inferno-style
##		flamegraph SVG, with no external flamegraph tool (no perl, no inferno,
##		no go-torch). It folds the stacks straight out of `go tool pprof -raw`
##		- leaf-first sample location lists, locations expanded through their
##		inlined-frame continuation lines - then emits the same fg:x / fg:w /
##		total_samples SVG that flame-report.py reads. The output is a real,
##		viewable flamegraph (warm palette, per-frame tooltips), root at the
##		bottom, hotter/wider frames drawn first.
##	Syntax:
##		pprof2flame.py --prof cpu.prof --out flame.svg [--go go] [--title STR]
##		  --prof FILE   the profile written by `go test -cpuprofile` (required)
##		  --out FILE    SVG to write (required)
##		  --go BIN      go binary to invoke for `go tool pprof -raw` (default: go)
##		  --title STR   title drawn at the top of the SVG
##		  --minwidth PX prune frames narrower than this many px (default 0.1)
##	Exit: 0 wrote the SVG, 2 non-fatal skip (no profile / pprof unparseable).
##	History: At bottom of script.

##	Copyright (c) 2026 Bubbles
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT


import argparse
import html
import os
import subprocess
import sys

STEP = 16  # row height in the SVG, px (a child sits at parent_y - STEP)
RECTH = 15  # drawn rect height, px
WIDTH = 1200  # image width, px
PADTOP = 54  # headroom for the title + summary line, px
PADBOT = 16  # bottom margin, px


def fSkip(msg):
	##	2 = non-fatal skip - matches the cicd profiler stage, which treats a
	##	missing/unparseable profile as a warning, not a failure.
	sys.stderr.write(f"pprof2flame: {msg}\n")
	sys.exit(2)


def fFold(prof, go):
	##	Parse `go tool pprof -raw` into {(root..leaf): samples}. The raw dump has a
	##	Samples block (each line "<count> <period>: locID locID ...", leaf first)
	##	and a Locations block (each "ID: 0xADDR M=n func file:line s=..", with
	##	indented continuation lines for inlined callers, innermost first).
	if not os.path.isfile(prof):
		fSkip(f"no profile: {prof}")
	try:
		raw = subprocess.run([go, "tool", "pprof", "-raw", prof], capture_output=True, text=True, check=True).stdout
	except (OSError, subprocess.CalledProcessError) as e:
		fSkip(f"go tool pprof failed: {e}")

	samples, locs = [], {}  # samples: (count, [locID...]); locs: id -> [func, ...]
	section, curid = "", None
	for line in raw.splitlines():
		low = line.strip()
		if low in ("Samples:", "Locations", "Mappings"):
			section = low.rstrip(":")
			continue
		if section == "Samples":
			if ":" not in line or not low[:1].isdigit():
				continue
			head, tail = line.split(":", 1)
			cnt = head.split()[0]
			ids = tail.split()
			if cnt.isdigit() and ids:
				samples.append((int(cnt), ids))
		elif section == "Locations":
			if not low:
				continue
			parts = line.split()
			if line.lstrip().startswith(tuple("0123456789")) and parts[0].endswith(":"):
				curid = parts[0][:-1]  # "12:" -> "12"
				# id, addr, M=n, FUNC, file... -> func is the 4th field
				locs[curid] = [parts[3]] if len(parts) > 3 else ["?"]
			elif curid is not None and parts:
				locs[curid].append(parts[0])  # inlined caller (further from the leaf)
		# Mappings ignored

	if not samples:
		fSkip(f"no samples parsed from {prof}")

	folded = {}
	for cnt, ids in samples:
		frames = []  # leaf first
		for lid in ids:
			frames.extend(locs.get(lid, ["?"]))
		frames.reverse()  # root first
		key = tuple(frames)
		folded[key] = folded.get(key, 0) + cnt
	return folded


def fTree(folded):
	##	Merge folded stacks into a tree under a synthetic "all" root. Each node:
	##	[name, value, {childname: node}]. value is inclusive sample count.
	root = ["all", 0, {}]
	for frames, cnt in folded.items():
		root[1] += cnt
		node = root
		for name in frames:
			kid = node[2].get(name)
			if kid is None:
				kid = [name, 0, {}]
				node[2][name] = kid
			kid[1] += cnt
			node = kid
	return root


def fColor(name):
	##	Deterministic warm "hot" palette (red-orange), keyed on the frame name so
	##	a rebuild recolors identically. Mirrors inferno's hot scheme ranges.
	h = 0
	for ch in name:
		h = (h * 31 + ord(ch)) & 0xFFFFFFFF
	r = 205 + h % 50
	g = (h // 50) % 230
	b = (h // 12000) % 55
	return f"rgb({r},{g},{b})"


def fLayout(root):
	##	Assign each node an (xoff, depth) in samples/rows. Children are laid left to
	##	right in name order (stable across runs) within the parent's sample span.
	frames = []  # (name, xoff, depth, w)

	def walk(node, xoff, depth):
		frames.append((node[0], xoff, depth, node[1]))
		cx = xoff
		for name in sorted(node[2]):
			kid = node[2][name]
			walk(kid, cx, depth + 1)
			cx += kid[1]

	walk(root, 0, 0)
	return frames


def fEmit(frames, total, title, minwidth):
	maxdepth = max(f[2] for f in frames)
	height = PADTOP + (maxdepth + 1) * STEP + PADBOT
	usable = WIDTH - 20  # 10px side margins
	minfrac = (minwidth / usable) if usable else 0

	out = []
	out.append('<?xml version="1.0" standalone="no"?>')
	out.append('<!DOCTYPE svg PUBLIC "-//W3C//DTD SVG 1.1//EN" "http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd">')
	out.append(
		f'<svg version="1.1" width="{WIDTH}" height="{height}" '
		f'viewBox="0 0 {WIDTH} {height}" xmlns="http://www.w3.org/2000/svg" '
		f'xmlns:fg="http://github.com/jonhoo/inferno" total_samples="{total}">'
	)
	out.append("<!--Flame graph stack visualization; fg:w = raw samples. Generated by pprof2flame.py.-->")
	out.append(f'<rect x="0" y="0" width="{WIDTH}" height="{height}" fill="rgb(255,255,255)"/>')
	out.append(
		f'<text x="{WIDTH / 2:.0f}" y="24" font-family="Verdana" font-size="17" '
		f'text-anchor="middle">{html.escape(title)}</text>'
	)

	for name, xoff, depth, w in frames:
		frac = w / total if total else 0
		if frac < minfrac:
			continue
		x = 10 + xoff / total * usable
		y = PADTOP + (maxdepth - depth) * STEP
		pct = frac * 100
		esc = html.escape(name)
		out.append(
			f"<title>{esc} ({int(w)} samples, {pct:.2f}%)</title>"
			f'<rect x="{x / WIDTH * 100:.4f}%" y="{y}" width="{frac * usable / WIDTH * 100:.4f}%" '
			f'height="{RECTH}" fill="{fColor(name)}" fg:x="{int(xoff)}" fg:w="{int(w)}"/>'
		)
	out.append("</svg>")
	return "\n".join(out) + "\n"


def main():
	ap = argparse.ArgumentParser(description="Fold a Go CPU profile into an inferno-style flamegraph SVG.")
	ap.add_argument("--prof", required=True, help="profile from `go test -cpuprofile`")
	ap.add_argument("--out", required=True, help="SVG path to write")
	ap.add_argument("--go", default="go", help="go binary (default: go)")
	ap.add_argument("--title", default="shclwrap CPU flamegraph", help="SVG title")
	ap.add_argument("--minwidth", type=float, default=0.1, help="prune frames narrower than this many px")
	a = ap.parse_args()

	folded = fFold(a.prof, a.go)
	root = fTree(folded)
	frames = fLayout(root)
	svg = fEmit(frames, root[1], a.title, a.minwidth)
	try:
		with open(a.out, "w", encoding="utf-8") as fh:
			fh.write(svg)
	except OSError as e:
		fSkip(f"could not write {a.out}: {e}")
	print(f"pprof2flame: wrote {a.out}  ({root[1]} samples, {max(f[2] for f in frames)} deep)")


if __name__ == "__main__":
	main()


##	History:
##		- 20260709: Created.
##		- 2026-10-25 JC: Copied from zuid for shclwrap.
