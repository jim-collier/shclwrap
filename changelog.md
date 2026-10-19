# Changelog

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

<!--
## vNEXT - DATE

### Notes

### Added

### Changed

### Removed

### Other work
-->

## Unreleased

### Added

- Go and Rust wrappers over shcl 1.2, 2.0, and 3.0, with one API across all of them.

- Ops scripts, a text form of reads and edits, through `shclw.Run` and `shclw::run`.

- A pipeline with shared test cases for both languages, and fuzzing of Go against Rust.

- `First`, `Last`, and `Lines`, to read one instance of a key that's set more than once, and to say where the others are.

- `Open` upgrades a 1.2 or 2.0 file in memory, and the first `Write` over it backs up the original. `UpgradedFrom` and `BackupName` say so.

### Changed

- The project is now named shclwrap, and the Go module path is `github.com/jim-collier/shclwrap/source/go`.
