# Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
# SPDX-License-Identifier: MIT

# The API calls the shared cases don't reach. Run from source/python with shcl
# 3.0's folder on PYTHONPATH: python3 -m unittest discover -s tests

from __future__ import annotations

import errno
import math
import os
import subprocess
import sys
import tempfile
import threading
import unittest
from collections.abc import Callable
from pathlib import Path
from unittest import mock

import shclw

HERE = Path(__file__).resolve().parent.parent


class FindsShcl(unittest.TestCase):
	def refused(self, body: str) -> str:
		with tempfile.TemporaryDirectory() as tmp:
			Path(tmp, "shcl.py").write_text(body, encoding="utf-8")
			env = dict(os.environ, PYTHONPATH=f"{tmp}{os.pathsep}{HERE}")
			p = subprocess.run(
				[sys.executable, "-c", "import shclw"],
				env=env,
				capture_output=True,
				encoding="utf-8",
				errors="replace",
				cwd=tmp,
				check=False,
			)
		self.assertNotEqual(p.returncode, 0)
		return p.stderr

	# Test ID: de8es
	def test_older_refused(self) -> None:
		# 1.2 and 2.0 have no FORMAT_MAJOR.
		self.assertIn("older than 3.0", self.refused("VERSION = '2.0.0'\n"))

	# Test ID: de8eq
	def test_newer_refused(self) -> None:
		self.assertIn("format 4", self.refused("FORMAT_MAJOR = 4\n"))

	# Test ID: de8er
	def test_none(self) -> None:
		with tempfile.TemporaryDirectory() as tmp:
			p = subprocess.run(
				[sys.executable, "-I", "-c", f"import sys; sys.path.insert(0, {str(HERE)!r}); import shclw"],
				capture_output=True,
				encoding="utf-8",
				errors="replace",
				cwd=tmp,
				check=False,
			)
		self.assertIn("installed or copied beside", p.stderr)

	# Test ID: de8en
	def test_copied(self) -> None:
		# 3.0 copied beside the wrapper in a package, with an older shcl installed.
		installed = Path(shclw.shcl.__file__)
		with tempfile.TemporaryDirectory() as tmp:
			pkg = Path(tmp, "pkg")
			pkg.mkdir()
			Path(pkg, "__init__.py").write_text("", encoding="utf-8")
			for f in ("shclw.py", "_shclw_shcl2.py"):
				Path(pkg, f).write_bytes(Path(HERE, f).read_bytes())
			Path(pkg, "shcl.py").write_bytes(installed.read_bytes())
			Path(tmp, "shcl.py").write_text("VERSION = '1.2.0'\n", encoding="utf-8")
			code = "import pkg.shclw as w, shcl; "
			code += "print(w.shcl.__name__, hasattr(shcl, 'FORMAT_MAJOR'), w.parse('a: 1').int_or('a', 0))"
			p = subprocess.run(
				[sys.executable, "-c", code],
				env=dict(os.environ, PYTHONPATH=tmp),
				capture_output=True,
				encoding="utf-8",
				errors="replace",
				cwd=tmp,
				check=False,
			)
		self.assertEqual(p.stdout.split(), ["_shclw_shcl3", "False", "1"], p.stderr)

	# Test ID: de8ep
	def test_found(self) -> None:
		self.assertEqual(shclw.FORMAT_MAJOR, 3)
		self.assertEqual(shclw.shcl.FORMAT_MAJOR, 3)
		self.assertFalse(hasattr(shclw.shcl2, "FORMAT_MAJOR"))


class Api(unittest.TestCase):
	def setUp(self) -> None:
		self.tmp = tempfile.TemporaryDirectory()
		self.dir = Path(self.tmp.name)

	def tearDown(self) -> None:
		self.tmp.cleanup()

	# Test ID: de8ej
	def test_reads(self) -> None:
		d = shclw.parse("a: 1\nb: x\nc: [1, 2]\nf: 1e400\n")
		self.assertEqual(d.get_int("a").value, 1)
		self.assertTrue(d.get_int("a").ok())
		self.assertEqual(d.str_or("b", "z"), "x")
		self.assertEqual(d.ints_or("c", []), [1, 2])
		self.assertEqual(d.int_or("nope", 7), 7)
		self.assertIs(d.get_float("f").status, shclw.Status.BadType)
		self.assertEqual(d.float_or("f", 2.5), 2.5)

	# Test ID: de8eh
	def test_put_kinds(self) -> None:
		d = shclw.new()
		d.put("i", 3)
		d.put("b", True)
		d.put("f", 0.5)
		d.put("s", ["a", "b"])
		d.put("n", (1, 2))
		self.assertEqual(d.get_bool("b").value, True)
		self.assertEqual(d.get_ints("n").value, [1, 2])
		for bad in (math.nan, [1.0, math.inf], [True], {"a": 1}, None):
			with self.assertRaises(shclw.Error) as e:
				d.put("x", bad)
			self.assertIs(e.exception.kind, shclw.ErrKind.BadValue)

	# Test ID: de8ek
	def test_strict(self) -> None:
		with self.assertRaises(shclw.Error) as e:
			shclw.parse_with("a: [1\n", shclw.Strictness.Strict)
		self.assertIs(e.exception.kind, shclw.ErrKind.Strict)
		self.assertTrue(e.exception.diags)

	# Test ID: de8eg
	def test_open_missing(self) -> None:
		with self.assertRaises(shclw.Error) as e:
			shclw.open(str(self.dir / "none.shcl"))
		self.assertIs(e.exception.kind, shclw.ErrKind.NotFound)

	# Test ID: de8em
	def test_upgrade_on_write(self) -> None:
		p = self.dir / "old.shcl"
		p.write_bytes((HERE.parent.parent / "tests/cases/036-open-upgrade/doc.shcl").read_bytes())
		d = shclw.open(str(p))
		self.assertEqual(d.upgraded_from(), 2)
		self.assertTrue(d.rewrites())
		self.assertEqual(d.backup_name(), "")
		d.write(str(p))
		self.assertTrue(Path(d.backup_name()).is_file())
		self.assertFalse(d.rewrites())
		self.assertEqual(shclw.format_of(p.read_bytes().decode("utf-8")), 3)
		self.assertEqual(shclw.open(str(p)).upgraded_from(), 0)

	# Test ID: de8ef
	def test_layers(self) -> None:
		(self.dir / "user.shcl").write_bytes(b"a: 2\n")
		d = shclw.open_layers(
			shclw.Layer(text="a: 1\nb: 1\n"), shclw.Layer(file=str(self.dir / "user.shcl")), shclw.Layer(text="c: 3\n")
		)
		self.assertEqual([d.int_or(k, 0) for k in "abc"], [2, 1, 3])
		d.put("b", 5)
		d.write(str(self.dir / "user.shcl"))
		self.assertEqual((self.dir / "user.shcl").read_bytes(), b"a: 2\n\nb: 5\n")

	# Test ID: de8ee
	def test_format_float(self) -> None:
		cases = [(0.0, "0.0"), (-0.0, "-0.0"), (1e17, "1e17"), (1e16, "10000000000000000.0"), (1e-5, "0.00001")]
		cases += [(1e-6, "1e-6"), (123.456, "123.456"), (5e-324, "5e-324"), (0.1, "0.1"), (-2.5e20, "-2.5e20")]
		for x, want in cases:
			self.assertEqual(shclw.format_float(x), want)


class CheckReads(unittest.TestCase):
	# Test ID: dejeh
	def test_repeated(self) -> None:
		# Each instance of a repeated key is read back against 2.0, so one
		# shcl split, as it did before a010fdf8, is refused on its own line
		# (2026102816332481).
		old = shclw.shcl2.Document.parse('row: idx=2, title="a, b"\nrow: idx=3\nt:\n\tx: 1\nt:\n\tx: 2\n')
		good = "row: [idx=2, 'title=\"a, b\"']\nrow: idx=3\nt:\n\tx: 1\nt:\n\tx: 2\n"
		self.assertEqual(shclw._check_reads(old, good, False), [])
		for up, line in (
			("row: [idx=2, 'title=\"a', 'b\"']\nrow: idx=3\nt:\n\tx: 1\nt:\n\tx: 2\n", 1),
			("row: [idx=2, 'title=\"a, b\"']\nrow: idx=4\nt:\n\tx: 1\nt:\n\tx: 2\n", 2),
			("row: [idx=2, 'title=\"a, b\"']\nrow: idx=3\nt:\n\tx: 1\nt:\n\tx: 3\n", 6),
		):
			with self.subTest(up=up):
				with self.assertRaises(shclw.Error) as e:
					shclw._check_reads(old, up, False)
				self.assertEqual(e.exception.kind, shclw.ErrKind.Refused)
				self.assertEqual([d.line for d in e.exception.diags], [line])
		# An instance 3.0 reads as empty is lost, as a single key's would be.
		up = "row: [idx=2, 'title=\"a, b\"']\nrow:\nt:\n\tx: 1\nt:\n\tx: 2\n"
		self.assertEqual(shclw._check_reads(old, up, False), [2])


if __name__ == "__main__":
	unittest.main()


class CreateRace(unittest.TestCase):
	"""Of several creates of one name at once, exactly one wins, and the file
	is all of the winner's text."""

	def race(self, create: Callable[[str, str, int | None], None]) -> None:
		for _ in range(20):
			with tempfile.TemporaryDirectory() as tmp:
				path = os.path.join(tmp, "a.shcl")
				start = threading.Barrier(8)
				outcome: dict[int, str] = {}

				def racer(i: int, path: str, start: threading.Barrier, outcome: dict[int, str]) -> None:
					start.wait()
					try:
						create(path, f"v: {i}\n" * 5000, 0o600)
						outcome[i] = "won"
					except shclw.Error as e:
						outcome[i] = e.kind.value

				threads = [threading.Thread(target=racer, args=(i, path, start, outcome)) for i in range(8)]
				for t in threads:
					t.start()
				for t in threads:
					t.join()
				won = [i for i, o in outcome.items() if o == "won"]
				self.assertEqual(len(won), 1, outcome)
				self.assertEqual(sorted(set(outcome.values())), ["exists", "won"])
				self.assertEqual(Path(path).read_bytes(), f"v: {won[0]}\n".encode() * 5000)
				self.assertEqual(os.listdir(tmp), ["a.shcl"])
				if os.name != "nt":
					self.assertEqual(os.stat(path).st_mode & 0o777, 0o600)

	# Test ID: de8hx
	def test_link(self) -> None:
		self.race(shclw.create_new)

	# Test ID: de8hw
	def test_in_place(self) -> None:
		self.race(shclw._create_in_place)  # noqa: SLF001 - the no-hard-link path


def _fat_link_error() -> OSError:
	"""What os.link raises on FAT on Windows, built the same way elsewhere."""
	e = OSError(errno.EINVAL, "Incorrect function")
	e.winerror = 1  # type: ignore[attr-defined] - only Windows sets it
	return e


class CreateNoLinks(unittest.TestCase):
	"""A link error that says the filesystem has no hard links falls back to
	the exclusive create, and any other link error is io."""

	# Test ID: dejdh
	def test_no_links(self) -> None:
		cases: list[tuple[OSError, str | None]] = [
			(_fat_link_error(), None),
			(PermissionError(errno.EPERM, "Operation not permitted"), None),
			(OSError(errno.ENOTSUP, "Not supported"), None),
			(OSError(errno.EIO, "I/O error"), "io"),
		]
		for err, want in cases:
			link = mock.patch.object(os, "link", side_effect=err)
			with self.subTest(err=err), tempfile.TemporaryDirectory() as tmp, link:
				path = os.path.join(tmp, "a.shcl")
				try:
					shclw.create_new(path, "a: 1\n")
					got = None
				except shclw.Error as e:
					got = e.kind.value
				self.assertEqual(got, want)
				if want is None:
					self.assertEqual(Path(path).read_bytes(), b"a: 1\n")
					self.assertEqual(os.listdir(tmp), ["a.shcl"])
				else:
					self.assertEqual(os.listdir(tmp), [])


class WriteHeldOpen(unittest.TestCase):
	"""On Windows a file held open without delete sharing, as open() holds it,
	can't be replaced. The save fails, and leaves the old file whole with
	nothing beside it, for a current file, one open() upgraded, and
	upgrade_file()."""

	# Test ID: dejfz
	@unittest.skipUnless(os.name == "nt", "only Windows blocks a rename over an open file")
	def test_held(self) -> None:
		for name, text in (("current", b"a: 1\n"), ("upgraded", b"tags: a, b\n")):
			with self.subTest(name=name), tempfile.TemporaryDirectory() as tmp:
				path = os.path.join(tmp, "c.shcl")
				Path(path).write_bytes(text)
				d = shclw.open(path)
				d.put("n", 1)
				with open(path, "rb"), self.assertRaises(shclw.Error) as e:
					d.write(path)
				self.assertIs(e.exception.kind, shclw.ErrKind.IO)
				self.assertEqual(Path(path).read_bytes(), text)
				self.assertEqual(os.listdir(tmp), ["c.shcl"])
				self.assertEqual(d.backup_name(), "")
				d.write(path)
				self.assertIn(b"n: 1", Path(path).read_bytes())
		with self.subTest(name="upgrade_file"), tempfile.TemporaryDirectory() as tmp:
			path = os.path.join(tmp, "c.shcl")
			Path(path).write_bytes(b"tags: a, b\n")
			with open(path, "rb"), self.assertRaises(shclw.Error) as e:
				shclw.upgrade_file(path, 2)
			self.assertIs(e.exception.kind, shclw.ErrKind.IO)
			self.assertEqual(Path(path).read_bytes(), b"tags: a, b\n")
			self.assertEqual(os.listdir(tmp), ["c.shcl"])
			shclw.upgrade_file(path, 2)
			self.assertEqual(len(os.listdir(tmp)), 2)
