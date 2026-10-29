// Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
// SPDX-License-Identifier: MIT

//! Saves and upgrades on real files: refusals, a taken backup name, a file
//! that changes underneath, and a write that fails.

use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use shclw::{replace_read, ErrKind, Error, ShclDoc};

mod common;
use common::Scratch;

const CLEAN_2X: &str = "tags: a, b\n";
const CLEAN_2TO: &str = "tags: [a, b]\n";

fn s(p: &Path) -> String {
	p.to_string_lossy().into_owned()
}

fn read(p: &Path) -> String {
	fs::read_to_string(p).unwrap_or_else(|e| panic!("{}: {e}", p.display()))
}

fn names(dir: &Path) -> Vec<String> {
	let mut out: Vec<String> = fs::read_dir(dir)
		.unwrap()
		.map(|e| e.unwrap().file_name().to_string_lossy().into_owned())
		.collect();
	out.sort();
	out
}

#[cfg(unix)]
fn set_mode(p: &Path, mode: u32) {
	use std::os::unix::fs::PermissionsExt;
	fs::set_permissions(p, fs::Permissions::from_mode(mode)).unwrap();
}

#[cfg(unix)]
fn mode(p: &Path) -> u32 {
	use std::os::unix::fs::PermissionsExt;
	fs::metadata(p).unwrap().permissions().mode() & 0o777
}

/// Makes a folder writable again on drop, so the scratch folder made before
/// it can still be removed after a failed assert.
#[cfg(unix)]
struct Unlock(PathBuf);

#[cfg(unix)]
impl Drop for Unlock {
	fn drop(&mut self) {
		set_mode(&self.0, 0o755);
	}
}

/// Locks `dir` so nothing can be added to it or renamed in it, or `None`
/// where the filesystem or the user ignores the mode.
#[cfg(unix)]
fn read_only(dir: &Path) -> Option<Unlock> {
	set_mode(dir, 0o555);
	let lock = Unlock(dir.to_path_buf());
	let probe = dir.join("probe");
	if fs::write(&probe, "").is_ok() {
		let _ = fs::remove_file(&probe);
		eprintln!("skipped: a read-only folder still takes new files here");
		return None;
	}
	Some(lock)
}

fn io_err(msg: &str) -> Error {
	Error::new(ErrKind::Io).msg(msg)
}

/// A folder with `c.shcl` holding `was`, and a backup that copies it to
/// `c.bak`.
struct Setup {
	_scratch: Scratch,
	dir: PathBuf,
	path: String,
	bak: PathBuf,
}

const WAS: &str = "a: 1\n";
const NOW: &str = "a: 2\n";

fn setup(name: &str) -> Setup {
	let scratch = Scratch::new(name);
	let dir = scratch.path().to_path_buf();
	let path = dir.join("c.shcl");
	fs::write(&path, WAS).unwrap();
	#[cfg(unix)]
	set_mode(&path, 0o640);
	Setup {
		bak: dir.join("c.bak"),
		path: s(&path),
		dir,
		_scratch: scratch,
	}
}

impl Setup {
	fn backup(&self) -> impl FnOnce() -> Result<String, Error> + '_ {
		|| {
			// A real backup has the original's mode.
			fs::write(&self.bak, WAS).unwrap();
			#[cfg(unix)]
			set_mode(&self.bak, 0o640);
			Ok(s(&self.bak))
		}
	}
}

fn plain(path: &str, data: &str) -> Result<(), Error> {
	fs::write(path, data).map_err(|e| io_err(&e.to_string()))
}

// Test ID: de010
#[test]
fn replace_read_ok() {
	let t = setup("replace-ok");
	let name = replace_read(&t.path, WAS, NOW, t.backup(), plain).expect("replace");
	assert_eq!(read(Path::new(&t.path)), NOW);
	assert_eq!(read(Path::new(&name)), WAS);
	let t = setup("replace-nobackup");
	assert_eq!(
		replace_read(&t.path, WAS, NOW, || Ok(String::new()), plain).unwrap(),
		""
	);
	assert_eq!(names(&t.dir), ["c.shcl"]);
}

// Test ID: de00z
#[test]
fn replace_read_changed() {
	let t = setup("replace-changed");
	let mut called = false;
	let e = replace_read(
		&t.path,
		"a: 0\n",
		NOW,
		|| {
			called = true;
			Ok(String::new())
		},
		plain,
	)
	.expect_err("changed before");
	assert_eq!(e.kind, ErrKind::Changed);
	assert!(!called, "backup made for a file that changed");
	assert_eq!(read(Path::new(&t.path)), WAS);

	let backup = t.backup();
	let edit = || {
		let name = backup()?;
		fs::write(&t.path, "a: 3\n").unwrap();
		Ok(name)
	};
	let e = replace_read(&t.path, WAS, NOW, edit, plain).expect_err("changed during backup");
	assert_eq!(e.kind, ErrKind::Changed);
	assert_eq!(read(Path::new(&t.path)), "a: 3\n");
	assert_eq!(names(&t.dir), ["c.shcl"]);
}

// Test ID: de00y
#[test]
fn replace_read_backup_fails() {
	let t = setup("replace-backupfails");
	let e = replace_read(&t.path, WAS, NOW, || Err(Error::new(ErrKind::BackupTaken)), plain).expect_err("backup");
	assert_eq!(e.kind, ErrKind::BackupTaken);
	assert_eq!(read(Path::new(&t.path)), WAS);
}

// Test ID: de011
#[test]
fn replace_read_write_fails() {
	let t = setup("replace-writefails");
	let e = replace_read(&t.path, WAS, NOW, t.backup(), |_, _| Err(io_err("boom"))).expect_err("write");
	assert_eq!((e.kind, e.msg.as_str()), (ErrKind::Io, "boom"));
	assert_eq!(read(Path::new(&t.path)), WAS);
	assert_eq!(names(&t.dir), ["c.shcl"]);

	// Nothing left at the path: the backup goes back in its place.
	let gone = |path: &str, _: &str| {
		fs::remove_file(path).unwrap();
		Err(io_err("boom"))
	};
	let e = replace_read(&t.path, WAS, NOW, t.backup(), gone).expect_err("write");
	assert_eq!(e.kind, ErrKind::Io);
	assert_eq!(read(Path::new(&t.path)), WAS);
	#[cfg(unix)]
	assert_eq!(mode(Path::new(&t.path)), 0o640);
	assert_eq!(names(&t.dir), ["c.shcl"]);

	// Something else at the path: the backup stays, and the error names it.
	let half = |path: &str, _: &str| {
		fs::write(path, "a:").unwrap();
		Err(io_err("boom"))
	};
	let e = replace_read(&t.path, WAS, NOW, t.backup(), half).expect_err("write");
	assert_eq!(e.kind, ErrKind::Io);
	assert!(e.msg.contains(&s(&t.bak)), "{e}");
	assert_eq!(read(&t.bak), WAS);
}

/// An edit made while `upgrade_file` runs must not get past the 2.0 check.
/// The file flips between a clean 2.x text and one 2.0 has errors on, so a
/// backup holding the bad text means that text was converted unchecked.
///
/// The flipping starts at a delay swept over what one upgrade takes on this
/// box. A flipper started with the upgrade lands its first flip in step with
/// it on a slow box, so every round there is refused (2026102818352324). An
/// edit after the last check is lost, as design.md says, so the late starts
/// let rounds finish.
// Test ID: de017
#[test]
fn upgrade_file_edited_meanwhile() {
	const BAD: &str = "x: [ab]\ny: 1\n";
	let scratch = Scratch::new("upgrade-race");
	// The first upgrade is slower, so the second one is timed.
	let mut took = Duration::ZERO;
	for name in ["cold", "timed"] {
		let dir = scratch.path().join(name);
		fs::create_dir(&dir).unwrap();
		let path = dir.join("c.shcl");
		fs::write(&path, CLEAN_2X).unwrap();
		let at = Instant::now();
		shclw::v3::upgrade_file(&s(&path), 2).expect("upgrade with no edit");
		took = at.elapsed();
	}
	let deadline = Instant::now() + Duration::from_secs(10);
	let (mut upgraded, mut raced) = (0, 0);
	for round in 0..120u32 {
		if Instant::now() > deadline {
			break;
		}
		let dir = scratch.path().join(round.to_string());
		fs::create_dir(&dir).unwrap();
		let path = dir.join("c.shcl");
		fs::write(&path, CLEAN_2X).unwrap();
		let flip_at = Instant::now() + took * (round % 12) / 10;
		let stop = AtomicBool::new(false);
		let got = std::thread::scope(|sc| {
			let flipper = sc.spawn(|| {
				while Instant::now() < flip_at {
					if stop.load(Ordering::Relaxed) {
						return;
					}
					std::thread::park_timeout(flip_at.saturating_duration_since(Instant::now()));
				}
				let tmp = dir.join("flip");
				for n in 0.. {
					if stop.load(Ordering::Relaxed) {
						return;
					}
					let text = if n % 2 == 0 { BAD } else { CLEAN_2X };
					if fs::write(&tmp, text).is_err() || fs::rename(&tmp, &path).is_err() {
						return;
					}
				}
			});
			let got = shclw::v3::upgrade_file(&s(&path), 2);
			stop.store(true, Ordering::Relaxed);
			flipper.thread().unpark();
			got
		});
		// Any other failure ends the round too, as it always has. Only these
		// two say the edit was seen.
		let u = match got {
			Ok(u) => u,
			Err(e) => {
				raced += usize::from(matches!(e.kind, ErrKind::Changed | ErrKind::Refused));
				continue;
			}
		};
		if u.backup.is_empty() {
			continue;
		}
		upgraded += 1;
		assert_eq!(
			read(Path::new(&u.backup)),
			CLEAN_2X,
			"round {round}: that text was converted"
		);
	}
	assert!(upgraded > 0, "no round got as far as an upgrade, one takes {took:?}");
	assert!(raced > 0, "no round was edited meanwhile, one upgrade takes {took:?}");
}

// Test ID: de018
#[test]
fn upgrade_real_file() {
	let scratch = Scratch::new("upgrade-real");
	let dir = scratch.path();
	let path = dir.join("c.shcl");
	fs::write(&path, CLEAN_2X).unwrap();
	#[cfg(unix)]
	set_mode(&path, 0o600);
	let u = shclw::v3::upgrade_file(&s(&path), 2).expect("upgrade");
	assert!(u.changed && u.from == 2, "{u:?}");
	assert!(u.text.starts_with(CLEAN_2TO), "{u:?}");
	assert_eq!(shclw::v3::format_of(&u.text), 3);
	assert_eq!(read(&path), u.text);
	let backup = PathBuf::from(&u.backup);
	assert_eq!(backup.parent(), Some(dir));
	assert_eq!(read(&backup), CLEAN_2X);
	#[cfg(unix)]
	{
		assert_eq!(mode(&path), 0o600);
		assert_eq!(mode(&backup), 0o600);
	}
	assert_eq!(names(dir).len(), 2);
	let again = shclw::v3::upgrade_file(&s(&path), 0).expect("second run");
	assert!(!again.changed && again.backup.is_empty(), "{again:?}");
	assert_eq!(names(dir).len(), 2);
	let newer = dir.join("n.shcl");
	fs::write(&newer, "a: 1\n##    Format   9\n").unwrap();
	let e = shclw::v3::upgrade_file(&s(&newer), 0).expect_err("format 9 file");
	assert_eq!(e.kind, ErrKind::TooNew, "{e}");
	assert_eq!(read(&newer), "a: 1\n##    Format   9\n");
}

/// Something already at the backup's name is never written over, and the
/// file stays as it was.
// Test ID: de016
#[test]
fn upgrade_backup_taken() {
	let scratch = Scratch::new("upgrade-taken");
	let dir = scratch.path();
	let path = dir.join("c.shcl");
	let p = s(&path);
	// The name has the time in it to the second, so that way a tick between
	// taking it and the upgrade makes another. Go again when that happens.
	for _ in 0..3 {
		for n in names(dir) {
			fs::remove_file(dir.join(n)).unwrap();
		}
		fs::write(&path, CLEAN_2X).unwrap();
		let name = shclw::v3::shcl::backup_file_name(&p, 2);
		fs::write(&name, "mine\n").unwrap();
		let got = shclw::v3::upgrade_file(&p, 2);
		if shclw::v3::shcl::backup_file_name(&p, 2) != name {
			continue;
		}
		let e = got.expect_err("upgrade");
		assert_eq!(e.kind, ErrKind::BackupTaken, "{e}");
		assert_eq!(read(&path), CLEAN_2X);
		assert_eq!(read(Path::new(&name)), "mine\n");
		assert_eq!(names(dir).len(), 2);
		return;
	}
	panic!("the clock ticked on every try");
}

/// A save the folder won't take is `Io`, and the file there stays as it
/// was, on every version. So does an upgrade, which then leaves no backup.
// Test ID: de014
#[cfg(unix)]
#[test]
fn save_refused_by_folder() {
	let scratch = Scratch::new("save-folder");
	let dir = scratch.path();
	fs::write(dir.join("c.shcl"), WAS).unwrap();
	fs::write(dir.join("u.shcl"), CLEAN_2X).unwrap();
	let Some(_unlock) = read_only(dir) else { return };
	for name in ["c.shcl", "new.shcl"] {
		let p = s(&dir.join(name));
		let kind = |r: Result<(), Error>| r.err().map(|e| e.kind);
		assert_eq!(kind(shclw::v3::parse(NOW).write(&p)), Some(ErrKind::Io), "v3 {name}");
	}
	let e = shclw::v3::upgrade_file(&s(&dir.join("u.shcl")), 2).expect_err("upgrade");
	assert_eq!(e.kind, ErrKind::Io, "{e}");
	assert_eq!(read(&dir.join("c.shcl")), WAS);
	assert_eq!(read(&dir.join("u.shcl")), CLEAN_2X);
	assert_eq!(names(dir), ["c.shcl", "u.shcl"]);
}

/// A save that would drop lines the load couldn't use is refused, and the
/// file it was read from stays as it was.
// Test ID: de015
#[test]
fn save_refused_over_file() {
	let scratch = Scratch::new("save-refused");
	let dir = scratch.path();
	let path = dir.join("c.shcl");
	let p = s(&path);

	for text in ["a: 1\na(5): 2\n", "a: 1\na(5): 2\n##    Format   3\n"] {
		fs::write(&path, text).unwrap();
		let mut d = shclw::v3::open(&p).expect("open");
		d.remove("a");
		assert_eq!(d.write(&p).err().map(|e| e.kind), Some(ErrKind::Refused), "{text:?}");
		assert_eq!(read(&path), text, "{text:?}");
		assert_eq!(names(dir), ["c.shcl"]);
	}
}

/// `open` upgrades a 2.x file in memory and writes nothing. The first write
/// over it, by any name for the same file, backs up the original, and later
/// ones don't.
// Test ID: de5d5
#[test]
fn open_upgrades_in_memory() {
	let scratch = Scratch::new("open-upgrade");
	let dir = scratch.path();
	let path = dir.join("c.shcl");
	let p = s(&path);
	fs::write(&path, CLEAN_2X).unwrap();
	#[cfg(unix)]
	set_mode(&path, 0o600);
	let mut d = shclw::v3::open(&p).expect("open");
	assert_eq!((d.upgraded_from(), d.backup_name(), d.rewrites()), (2, "", true));
	assert_eq!(d.strs("tags", &[]), ["a", "b"]);
	assert_eq!(read(&path), CLEAN_2X);
	assert_eq!(names(dir), ["c.shcl"]);

	d.write(&s(&dir.join(".").join("c.shcl"))).expect("write");
	let backup = PathBuf::from(d.backup_name());
	assert_eq!(backup.parent(), Some(dir));
	assert_eq!(read(&backup), CLEAN_2X);
	#[cfg(unix)]
	assert_eq!(mode(&backup), 0o600);
	assert!(read(&path).starts_with(CLEAN_2TO));
	assert_eq!(shclw::v3::format_of(&read(&path)), 3);
	assert!(!d.rewrites());

	d.put("n", 1).expect("put");
	d.write(&p).expect("second write");
	assert_eq!(names(dir).len(), 2);
	assert_eq!(PathBuf::from(d.backup_name()), backup);
	let again = shclw::v3::open(&p).expect("reopen");
	assert_eq!((again.upgraded_from(), again.int("n", 0)), (0, 1));
}

/// A file that changed since `open` read it is left alone, with no backup.
// Test ID: de5d4
#[test]
fn open_upgrade_edited_meanwhile() {
	let scratch = Scratch::new("open-edited");
	let dir = scratch.path();
	let path = dir.join("c.shcl");
	let p = s(&path);
	fs::write(&path, CLEAN_2X).unwrap();
	let mut d = shclw::v3::open(&p).expect("open");
	fs::write(&path, "tags: x, y\n").unwrap();
	assert_eq!(d.write(&p).err().map(|e| e.kind), Some(ErrKind::Changed));
	assert_eq!(read(&path), "tags: x, y\n");
	assert_eq!(names(dir), ["c.shcl"]);
	assert_eq!(d.backup_name(), "");
}

/// Something already at the backup's name is never written over. The save
/// is `BackupTaken` and the file stays as it was.
// Test ID: de5d3
#[test]
fn open_upgrade_backup_taken() {
	let scratch = Scratch::new("open-taken");
	let dir = scratch.path();
	let path = dir.join("c.shcl");
	let p = s(&path);
	// The name has the time in it to the second. Go again on a tick.
	for _ in 0..3 {
		for n in names(dir) {
			fs::remove_file(dir.join(n)).unwrap();
		}
		fs::write(&path, CLEAN_2X).unwrap();
		let mut d = shclw::v3::open(&p).expect("open");
		let name = shclw::v3::shcl::backup_file_name(&p, 2);
		fs::write(&name, "mine\n").unwrap();
		let got = d.write(&p);
		if shclw::v3::shcl::backup_file_name(&p, 2) != name {
			continue;
		}
		assert_eq!(got.err().map(|e| e.kind), Some(ErrKind::BackupTaken));
		assert_eq!(d.backup_name(), "");
		assert_eq!(read(&path), CLEAN_2X);
		assert_eq!(read(Path::new(&name)), "mine\n");
		assert_eq!(names(dir).len(), 2);
		return;
	}
	panic!("the clock ticked on every try");
}

const BLOCK_2X: &str = "#\r\n# This config file format is SHCL.\r\n# \"Simple Hierarchical Config Language\"\r\n#    Home     https://github.com/jim-collier/shcl\r\n#\r\n";

/// A save of a file with 2.x's block and no Format line puts the current
/// block where it sat, with its Format line, so the next `open` reads what
/// was put rather than take the file as 2.x (2026101513330010).
// Test ID: de5kq
#[test]
fn write_swaps_old_block() {
	let scratch = Scratch::new("write-swap");
	let path = scratch.path().join("c.shcl");
	let p = s(&path);
	let text = format!("# app\r\nname: demo\r\n\r\n{BLOCK_2X}\r\nport: 80\r\n");
	let mut d = shclw::v3::parse(&text);
	d.put("dir", r"C:\temp").expect("put");
	d.write(&p).expect("write");
	let got = read(&path);
	assert_eq!(got, d.text());
	let at = got.find("##\r\n## This config file format is SHCL.").expect("block");
	assert!(at < got.find("port:").unwrap(), "{got:?}");
	assert_eq!(shclw::v3::format_of(&got), 3, "{got:?}");
	assert!(!got.contains("# This config file format is SHCL.\r\n# "), "{got:?}");
	assert_eq!(got.matches('\n').count(), got.matches("\r\n").count(), "{got:?}");
	let again = shclw::v3::open(&p).expect("reopen");
	assert_eq!(again.upgraded_from(), 0);
	assert_eq!(again.str("dir", ""), r"C:\temp");

	// A block's text in a raw body is the body's.
	let raw = format!("tmpl:\n\t~~~\n{}\t~~~\n", BLOCK_2X.replace("\r\n", "\n"));
	shclw::v3::parse(&raw).write(&p).expect("raw body");
	assert_eq!(read(&path), raw);
	// A Format line already says what the file is.
	let stamped = format!("{}##    Format   3\n", BLOCK_2X.replace("\r\n", "\n"));
	shclw::v3::parse(&stamped).write(&p).expect("stamped");
	assert_eq!(read(&path), stamped);
}

/// A file with no Format line or 2.x block that has errors on 3.0 and 2.0
/// alike is current, with 3.0's errors in `diags`. One marked as 2.x is
/// still refused (2026101613005800).
// Test ID: de5kp
#[test]
fn open_fails_both() {
	const BOTH: &str = "a: 1\nb: ```\nraw line\nc: 2\n";
	let scratch = Scratch::new("fails-both");
	let dir = scratch.path();
	let path = dir.join("c.shcl");
	let p = s(&path);
	fs::write(&path, BOTH).unwrap();
	let d = shclw::v3::open(&p).expect("open");
	assert_eq!(d.upgraded_from(), 0);
	assert!(!d.rewrites());
	assert_eq!(d.int("a", 0), 1);
	assert_eq!(d.diags(), shclw::v3::parse(BOTH).diags());
	assert!(!d.diags().is_empty());
	let strict = shclw::v3::open_with(&p, shclw::Strictness::Strict);
	assert_eq!(strict.err().map(|e| e.kind), Some(ErrKind::Strict));
	let u = shclw::v3::upgrade(BOTH, 0).expect("upgrade 0");
	assert!(!u.changed && u.from == 3 && u.text == BOTH, "{u:?}");
	let u = shclw::v3::upgrade_file(&p, 0).expect("upgrade file");
	assert!(!u.changed, "{u:?}");
	assert_eq!(names(dir), ["c.shcl"]);
	let block = BLOCK_2X.replace("\r\n", "\n");
	for (name, text, from) in [
		("from 2", BOTH.to_owned(), 2),
		("2.x block", format!("{block}{BOTH}"), 0),
		("Format 2", format!("##    Format   2\n{BOTH}"), 0),
	] {
		let got = shclw::v3::upgrade(&text, from).err().map(|e| e.kind);
		assert_eq!(got, Some(ErrKind::Refused), "{name}: upgrade");
		if from == 0 {
			fs::write(&path, &text).unwrap();
			let got = shclw::v3::open(&p).err().map(|e| e.kind);
			assert_eq!(got, Some(ErrKind::Refused), "{name}: open");
		}
	}
}

/// Opens `p` without delete sharing, as most Windows programs open a file.
fn hold(p: &Path) -> fs::File {
	let mut o = fs::OpenOptions::new();
	o.read(true);
	#[cfg(windows)]
	{
		use std::os::windows::fs::OpenOptionsExt;
		const FILE_SHARE_READ: u32 = 1;
		const FILE_SHARE_WRITE: u32 = 2;
		o.share_mode(FILE_SHARE_READ | FILE_SHARE_WRITE);
	}
	o.open(p).unwrap_or_else(|e| panic!("{}: {e}", p.display()))
}

/// On Windows a file held open without delete sharing can't be replaced. The
/// save fails, and leaves the old file whole with nothing beside it, for a
/// current file, one `open` upgraded, and `upgrade_file`.
// Test ID: dejfy
#[test]
#[cfg_attr(not(windows), ignore = "only Windows blocks a rename over an open file")]
fn write_held_open() {
	for (name, text) in [("current", "a: 1\n"), ("upgraded", CLEAN_2X)] {
		let scratch = Scratch::new(&format!("held-open-{name}"));
		let dir = scratch.path();
		let path = dir.join("c.shcl");
		let p = s(&path);
		fs::write(&path, text).unwrap();
		let mut d = shclw::v3::open(&p).expect("open");
		d.put("n", 1).expect("put");
		let held = hold(&path);
		let got = d.write(&p).err().map(|e| e.kind);
		drop(held);
		assert_eq!(got, Some(ErrKind::Io), "{name}");
		assert_eq!(read(&path), text, "{name}");
		assert_eq!(names(dir), ["c.shcl"], "{name}");
		assert_eq!(d.backup_name(), "", "{name}");
		d.write(&p).unwrap_or_else(|e| panic!("{name}: write once let go: {e}"));
		assert!(read(&path).contains("n: 1"), "{name}");
	}
	let scratch = Scratch::new("held-open-upgrade-file");
	let dir = scratch.path();
	let path = dir.join("c.shcl");
	fs::write(&path, CLEAN_2X).unwrap();
	let held = hold(&path);
	let got = shclw::v3::upgrade_file(&s(&path), 2).err().map(|e| e.kind);
	drop(held);
	assert_eq!(got, Some(ErrKind::Io), "upgrade_file");
	assert_eq!(read(&path), CLEAN_2X);
	assert_eq!(names(dir), ["c.shcl"]);
	shclw::v3::upgrade_file(&s(&path), 2).expect("upgrade once let go");
	assert_eq!(names(dir).len(), 2);
}
