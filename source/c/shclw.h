// Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
// SPDX-License-Identifier: MIT

// shclw: a small, stable wrapper over the shcl version a program builds
// against. The calls are the same as the Go and Rust wrappers', in snake case.
// See project/design.md.
//
// Build: shclw.c, info.c, and shclw3.c, the adapter for shcl 3.0, with 3.0's
// shcl.h on the include path, and run.c for shclw_run. shcl.h's own
// SHCL_IMPLEMENTATION goes in one .c of the program's, as shcl asks.
//
// The build also needs shclw3_v2.c and shclw3_v2_impl.c, built with shcl
// 2.0's shcl.h on the include path in place of 3.0's. They're shcl 2.0 under
// other names, which the upgrade checks a 2.x file with. 2.0 ends the program
// when it runs out of memory.
//
// Include this before any system header, since shcl.h has to be first.
//
// Memory: the strings, arrays, text, diagnostics, and upgrades a shclw call
// hands back are malloc'd, each in one block, and the caller frees them with
// free().
// Documents are freed with shclw_free. Reads copy their value out and give
// shcl's read memory back before they return, and edits keep what they
// replaced until shclw_write, which compacts the document after a good save.
// A program that edits in a loop and never saves can call shclw_compact.
//
// Strings: anything shclw hands back has its length and a NUL after it. Go by
// the length, since a value can hold a NUL. Paths and file names are plain
// NUL-terminated strings.
//
// Errors: a call that can fail returns a shclw_kind, SHCLW_OK on success, and
// fills *err when err isn't NULL.
//
// A document is for one thread at a time, reads included, as in shcl.

#ifndef SHCLW_H
#define SHCLW_H

#include "shcl.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// shcl.h has no version macro before 3.0. SHCL_FORMAT_MAJOR is new in 3.0.
// SHCLW_SHCL_VERSION is the shcl the wrapper is tested against, since a
// shcl.h can't say its own.
#if !defined(SHCL_FORMAT_MAJOR)
#error "shclw: this shcl.h is 1.2 or 2.0, and shclw needs 3.0. shclw_upgrade moves their files forward."
#elif SHCL_FORMAT_MAJOR != 3
#error "shclw: this shcl.h is newer than 3.0, which shclw doesn't wrap yet"
#endif
#define SHCLW_FORMAT_MAJOR 3
#define SHCLW_SHCL_VERSION "3.0.0-dev+a010fdf8"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct shclw_doc shclw_doc;

typedef enum { SHCLW_GOOD, SHCLW_EMPTY, SHCLW_NOT_FOUND, SHCLW_BAD_TYPE, SHCLW_MULTIPLE, SHCLW_BAD_PATH } shclw_status;

typedef enum { SHCLW_LOOSE, SHCLW_STANDARD, SHCLW_STRICT } shclw_strictness;

typedef enum { SHCLW_SEV_ERROR, SHCLW_SEV_HINT } shclw_severity;

// The same kinds as Go and Rust, in the same order. SHCLW_ERR_NO_MEMORY is
// C's own.
typedef enum {
	SHCLW_OK,
	SHCLW_ERR_NOT_FOUND,
	SHCLW_ERR_IO,
	SHCLW_ERR_STRICT,
	SHCLW_ERR_REFUSED,
	SHCLW_ERR_BAD_PATH,
	SHCLW_ERR_BAD_VALUE,
	SHCLW_ERR_TOO_NEW,
	SHCLW_ERR_UNSUPPORTED,
	SHCLW_ERR_AMBIGUOUS,
	SHCLW_ERR_BACKUP_TAKEN,
	SHCLW_ERR_NO_READ_BACK,
	SHCLW_ERR_CHANGED,
	SHCLW_ERR_EXISTS,
	SHCLW_ERR_NO_MEMORY
} shclw_kind;

// msg reads like Go's Error(): kind, then the path and why. sys_errno is the
// errno behind an SHCLW_ERR_IO, or 0.
typedef struct {
	shclw_kind kind;
	int sys_errno;
	char msg[512];
} shclw_error;

// A string with its length. p has a NUL at p[n], and may hold others before it.
typedef struct {
	const char *p;
	size_t n;
} shclw_string;

// layer is the 1-based place in shclw_open_layers' list of the layer the line
// is in, or 0 for a document that isn't layered.
typedef struct {
	size_t line;
	shclw_severity severity;
	shclw_string code;
	shclw_string message;
	size_t layer;
} shclw_diag;

// A line in a loaded document. layer is as in shclw_diag.
typedef struct {
	size_t line;
	size_t layer;
} shclw_spot;

// A name shcl kept one copy of, since a later copy had the same name and value
// and was folded into it. lines has the kept copy's line, then each later
// copy's. path reads the kept copy, as a shclw_node's does, and layer is as in
// shclw_diag.
typedef struct {
	shclw_string path;
	const size_t *lines;
	size_t n_lines;
	size_t layer;
} shclw_merge;

// A comment line that reads as one key: value once its # comes off, such as a
// setting left in the file at its default. path is what the line would set.
// is_default says the line ends in a "## Default" note, which is the line a
// put of path fills in.
typedef struct {
	shclw_string path;
	shclw_string value;
	size_t line;
	bool is_default;
} shclw_commented_line;

// One layer for shclw_open_layers: a file, or with file NULL, text the program
// has, such as its defaults or this run's command line.
typedef struct {
	const char *file;
	const char *text;
	size_t len;
} shclw_layer;

// A read with its status. Check status before trusting value: an array that
// reads as SHCLW_BAD_TYPE still has its items, with 0 for the bad ones. line
// is 0 unless the path names one node. A value pointer is the caller's to
// free(), and is NULL only when memory ran out.
typedef struct {
	int64_t value;
	shclw_status status;
	size_t line;
} shclw_got_int;

typedef struct {
	double value;
	shclw_status status;
	size_t line;
} shclw_got_float;

typedef struct {
	bool value;
	shclw_status status;
	size_t line;
} shclw_got_bool;

typedef struct {
	char *value;
	size_t len;
	shclw_status status;
	size_t line;
} shclw_got_str;

// The items' text is in the same block as the array, so one free(value) frees
// all of it.
typedef struct {
	shclw_string *value;
	size_t n;
	shclw_status status;
	size_t line;
} shclw_got_strs;

typedef struct {
	int64_t *value;
	size_t n;
	shclw_status status;
	size_t line;
} shclw_got_ints;

typedef struct {
	double *value;
	size_t n;
	shclw_status status;
	size_t line;
} shclw_got_floats;

// One instance from shclw_each. path reads it and no other; add names to it
// with shclw_child. line is 0 for one a put made.
typedef struct {
	shclw_string path;
	shclw_string value;
	size_t line;
} shclw_node;

// "Good", "NotFound", and so on, as the other wrappers print them.
const char *shclw_status_name(shclw_status s);
// "notfound", "io", and so on.
const char *shclw_kind_name(shclw_kind k);
// "error" or "hint".
const char *shclw_severity_name(shclw_severity s);

// An empty document. NULL only when memory ran out.
shclw_doc *shclw_new(void);
// Never fails on the text's account; problems are in shclw_diags. NULL only
// when memory ran out. text needn't end in a NUL.
shclw_doc *shclw_parse(const char *text, size_t len);
// Fails only at SHCLW_STRICT, with SHCLW_ERR_STRICT. *doc is set either way.
shclw_kind shclw_parse_with(const char *text, size_t len, shclw_strictness level, shclw_doc **doc, shclw_error *err);
// Reads and parses a file. A missing file is SHCLW_ERR_NOT_FOUND. One written
// for a newer format is read as this shcl reads it, and is read-only:
// shclw_write on it is SHCLW_ERR_TOO_NEW. An older file is upgraded in
// memory, as shclw_upgrade with from 0 does, and shclw_upgraded_from says so.
// Nothing is written until shclw_write, and the first save over the file
// backs it up. A 2.x file that reads two ways is SHCLW_ERR_AMBIGUOUS, and one shcl
// 2.0 reports errors on, or that can't be made to read as 2.0 read it,
// SHCLW_ERR_REFUSED, with the lines in the empty document's shclw_diags. On a failure *doc is an empty document, so
// reads give their fallbacks. Free it either way.
shclw_kind shclw_open(const char *path, shclw_doc **doc, shclw_error *err);
// level applies to the upgraded text.
shclw_kind shclw_open_with(const char *path, shclw_strictness level, shclw_doc **doc, shclw_error *err);
// Reads n layers in order, lowest first, such as defaults, then a site file,
// then the user's, and reads as their merge: a later layer's value, array, or
// raw block takes the place of a lower one's, and blocks merge name by name,
// as shcl_merge does. A missing file is skipped. Edits and saves go to the
// last file layer, so shclw_text, shclw_write, and the rest are that layer's,
// and a save writes only it. Text layers after it, such as this run's command
// line, still win reads. Each file is opened as shclw_open does, and the first
// error is returned, with *doc the empty document shclw_open gives, its
// diagnostics with their layer set. shclw_diags gives every layer's, in layer
// order. Free *doc either way.
shclw_kind shclw_open_layers(const shclw_layer *layers, size_t n, shclw_doc **doc, shclw_error *err);
// The format shclw_open upgraded the file from, or 0 when it didn't upgrade
// it. Reads, lines, diagnostics, and the text are all of the upgraded text.
int64_t shclw_upgraded_from(shclw_doc *d);
// Where a shclw_write copied the file shclw_open read, or "" when none did.
// That's the first one over an upgraded file, or one with backup that would
// have lost lines. Caller frees. NULL only when memory ran out.
char *shclw_backup_name(shclw_doc *d, size_t *len);
void shclw_free(shclw_doc *d);

// The document as it would be saved. Caller frees.
char *shclw_text(shclw_doc *d, size_t *len);
// The document in canonical form. Caller frees.
char *shclw_canonical(shclw_doc *d, size_t *len);
// Saves shclw_text atomically. SHCLW_ERR_REFUSED when the save would lose
// lines the load couldn't use. It writes nothing when the info block or the
// Format line can't go in, since an edit made after shclw_info_block or
// shclw_stamp_format. After a good save it gives back the memory earlier
// edits left behind. The first save over a file shclw_open upgraded backs up
// the original first, as shclw_upgrade_file does. A save over the file
// shclw_open read leaves it alone with SHCLW_ERR_CHANGED when it changed on
// disk since, and a document read from a newer format is SHCLW_ERR_TOO_NEW.
// A save with no Format line has 2.x's info block swapped for the current one
// where it sat.
shclw_kind shclw_write(shclw_doc *d, const char *path, shclw_error *err);
// What a save does. Zeroed is a plain shclw_write. force writes over the file
// shclw_open read even when it changed on disk since. It doesn't write over a
// file in a newer format.
//
// create writes only where nothing is there yet, and is SHCLW_ERR_EXISTS
// otherwise, even with 2 programs creating the file at once. It never
// replaces, so there's no changed check for force to skip.
//
// mode is the permission bits a file this save creates gets, exactly, with no
// umask. 0 is a plain create's 0666 less the umask. A file that's already
// there keeps its own mode. Windows ignores it.
//
// backup writes over the file shclw_open read when the save would lose lines
// the load couldn't use, which is SHCLW_ERR_REFUSED otherwise. It copies the
// file aside first, as the upgrade's backup does, and shclw_backup_name names
// the copy. Later saves then don't refuse, since the lines are in the copy.
// Anywhere else there's no copy to make, and it's still SHCLW_ERR_REFUSED.
typedef struct {
	bool force;
	bool create;
	unsigned mode;
	bool backup;
} shclw_write_opts;
// shclw_write with options, which may be NULL.
shclw_kind shclw_write_with(shclw_doc *d, const char *path, const shclw_write_opts *opts, shclw_error *err);
// Whether the document was read from a newer format, which shclw_write
// refuses with SHCLW_ERR_TOO_NEW.
bool shclw_read_only(shclw_doc *d);
// Gives back what earlier edits left behind, as a good shclw_write does, for a
// program that edits a lot between saves. C only, since Go and Rust free it
// themselves.
void shclw_compact(shclw_doc *d);

// Whether shclw_write puts the canonical form in place of the lines the
// document was loaded from. 3.0 does that when it can't keep them with the
// edits in, such as after a child is added under a flat dotted line, and
// comments can move. It builds the save's text to find out. A file shclw_open
// upgraded is rewritten until the first save over it.
bool shclw_rewrites(shclw_doc *d);

// The load's diagnostics. Caller frees the block. NULL only when memory ran out.
shclw_diag *shclw_diags(shclw_doc *d, size_t *n);
// How many of them are errors.
size_t shclw_errors(shclw_doc *d);
// The lines the load couldn't use, which are the lines with an error, once
// each, by layer then line. Not the 2.0 errors a refused shclw_open leaves in
// shclw_diags, since Go and Rust have those on the error. Caller frees. NULL
// only when memory ran out.
shclw_spot *shclw_unused(shclw_doc *d, size_t *n);
// Each name the load kept one copy of, since a later copy had the same name
// and value, with every copy's line. On a layered document it's each layer's
// own, and not what the layers' merge folded. One block; caller frees. NULL
// only when memory ran out.
shclw_merge *shclw_merged(shclw_doc *d, size_t *n);
// Each comment line that reads as one key: value once its # comes off, with
// the path it would set. A layered document's are the edit layer's. One block;
// caller frees. NULL only when memory ran out.
shclw_commented_line *shclw_commented(shclw_doc *d, size_t *n);
// d checked against a schema document. Caller frees the block. NULL only when
// memory ran out.
shclw_diag *shclw_check(shclw_doc *d, shclw_doc *schema, size_t *n);
// Takes the repeat and reopen hints for keys schema says repeat out of d's
// diagnostics, so shclw_diags gives fewer from then on. Go and Rust take a
// list and give one back; shcl's C binding only filters a document's own.
void shclw_drop_declared(shclw_doc *schema, shclw_doc *d);

// The format a file's Format line names, or 0 with none.
int64_t shclw_format_of(const char *text, size_t len);

// What an upgrade made of a file, in one block the caller frees with free().
// from is the format it was written for. unsure counts lines the upgrade
// couldn't convert for sure. backup names the copy shclw_upgrade_file made,
// and is "" with none. diags is shcl 2.0's errors on a file refused for them,
// which Go and Rust put on the error. lost lists the lines of the original,
// from 1, with a value the old shcl read that the upgraded file doesn't have.
// rewritten lists the rest of the original's lines that the upgraded text
// doesn't have as typed. A line that only moved isn't one.
typedef struct {
	shclw_string text;
	int64_t from;
	bool changed;
	size_t unsure;
	shclw_string backup;
	shclw_diag *diags;
	size_t n_diags;
	size_t *lost;
	size_t n_lost;
	size_t *rewritten;
	size_t n_rewritten;
} shclw_upgraded;

// Converts a file written for an older format. With from 0 the file's Format
// line decides. A file with none is taken as 2.x when it has 2.x's info
// block, or errors on 3.0 that shcl 2.0 doesn't have, and otherwise as
// current. Pass from 2 for a file known to be from shcl 2.x or older. A 2.x
// file that reads two ways is SHCLW_ERR_AMBIGUOUS, and one shcl 2.0
// reports errors on is SHCLW_ERR_REFUSED, with them in its diags. A 2.x file
// comes out reading what 2.0 read, backslash escapes included, and one that
// can't is SHCLW_ERR_REFUSED too. 2.x's info block is swapped for the
// current one where it sits, and the lines end as the file's did. Each
// line keeps its number, blank ones included.
// *out is set on a failure too, and is NULL only when memory ran out.
shclw_kind shclw_upgrade(const char *text, size_t len, int64_t from, shclw_upgraded **out, shclw_error *err);
// shclw_upgrade on a file. When it changes the file it first copies the
// original beside it. A file that changed after it was read is left alone with
// SHCLW_ERR_CHANGED, and a failed write leaves the original in place.
shclw_kind shclw_upgrade_file(const char *path, int64_t from, shclw_upgraded **out, shclw_error *err);

// Puts the info block at the end of what the save writes, or with on false
// leaves it out. Either way an old block comes off first, in any spelling
// shcl has written, wherever it is. A file whose Format line names another
// format is refused, and so is one that ends in a raw block that never closes.
// Reads and diagnostics stay as they were.
shclw_kind shclw_info_block(shclw_doc *d, bool on, shclw_error *err);
// Adds the Format line at the end of what the save writes, unless a comment
// already names this format. Refuses what shclw_info_block refuses.
shclw_kind shclw_stamp_format(shclw_doc *d, shclw_error *err);

// The value, or the fallback when there's no good one.
int64_t shclw_int(shclw_doc *d, const char *path, int64_t fallback);
double shclw_float(shclw_doc *d, const char *path, double fallback);
bool shclw_bool(shclw_doc *d, const char *path, bool fallback);
// A copy of the value or the fallback, which may be NULL for "". Caller frees.
char *shclw_str(shclw_doc *d, const char *path, const char *fallback, size_t fallback_len, size_t *len);
char *shclw_raw(shclw_doc *d, const char *path, const char *fallback, size_t fallback_len, size_t *len);
// Arrays copy the same way, as one block. Caller frees.
shclw_string *shclw_strs(shclw_doc *d, const char *path, const shclw_string *fallback, size_t fallback_n, size_t *n);
int64_t *shclw_ints(shclw_doc *d, const char *path, const int64_t *fallback, size_t fallback_n, size_t *n);
double *shclw_floats(shclw_doc *d, const char *path, const double *fallback, size_t fallback_n, size_t *n);

// The value with its status and line. A float that isn't finite is
// SHCLW_BAD_TYPE, as on every version.
shclw_got_int shclw_get_int(shclw_doc *d, const char *path);
shclw_got_float shclw_get_float(shclw_doc *d, const char *path);
shclw_got_bool shclw_get_bool(shclw_doc *d, const char *path);
shclw_got_str shclw_get_str(shclw_doc *d, const char *path);
shclw_got_str shclw_get_raw(shclw_doc *d, const char *path);
shclw_got_strs shclw_get_strs(shclw_doc *d, const char *path);
shclw_got_ints shclw_get_ints(shclw_doc *d, const char *path);
shclw_got_floats shclw_get_floats(shclw_doc *d, const char *path);

// Whether the path names anything, and how many instances.
bool shclw_has(shclw_doc *d, const char *path);
size_t shclw_count(shclw_doc *d, const char *path);
// The line of the one node path names, or 0.
size_t shclw_line(shclw_doc *d, const char *path);
// Child names under path, once each, in file order. One block; caller frees.
// NULL only when memory ran out.
shclw_string *shclw_names(shclw_doc *d, const char *path, size_t *n);
// Every instance path names, in the order shcl keeps them. A wildcard slot
// that reaches no node or several is left out. One block; caller frees.
shclw_node *shclw_each(shclw_doc *d, const char *path, size_t *n);
// The line of each instance, in file order. One a put made is 0, after the
// rest. Caller frees.
size_t *shclw_lines(shclw_doc *d, const char *path, size_t *n);
// path narrowed to its first or last instance by line, or a copy of path when
// it names none. Caller frees.
char *shclw_first(shclw_doc *d, const char *path, size_t *len);
char *shclw_last(shclw_doc *d, const char *path, size_t *len);
// base with one more name on the end, quoted if it needs it. base "" gives the
// name alone. Caller frees.
char *shclw_child(shclw_doc *d, const char *base, const char *name, size_t name_len, size_t *len);

// Puts read their value back: one that reads back different is
// SHCLW_ERR_NO_READ_BACK, with the edit left in place. A path with several
// instances at any step is SHCLW_ERR_BAD_PATH adn changes nothing.
shclw_kind shclw_put_int(shclw_doc *d, const char *path, int64_t v, shclw_error *err);
// Infinity and NaN are SHCLW_ERR_BAD_VALUE and change nothing.
shclw_kind shclw_put_float(shclw_doc *d, const char *path, double v, shclw_error *err);
// Written as the float's own shortest digits, so 0.9f is 0.9 and not the
// double it widens to.
shclw_kind shclw_put_float32(shclw_doc *d, const char *path, float v, shclw_error *err);
shclw_kind shclw_put_bool(shclw_doc *d, const char *path, bool v, shclw_error *err);
shclw_kind shclw_put_str(shclw_doc *d, const char *path, const char *v, size_t len, shclw_error *err);
shclw_kind shclw_put_strs(shclw_doc *d, const char *path, const shclw_string *v, size_t n, shclw_error *err);
shclw_kind shclw_put_ints(shclw_doc *d, const char *path, const int64_t *v, size_t n, shclw_error *err);
shclw_kind shclw_put_floats(shclw_doc *d, const char *path, const double *v, size_t n, shclw_error *err);
// text read as shcl value syntax. The whole text has to read back, so one
// with a trailing comment is SHCLW_ERR_NO_READ_BACK.
shclw_kind shclw_put_syntax(shclw_doc *d, const char *path, const char *text, size_t len, shclw_error *err);
// Sets the comment above path.
shclw_kind shclw_comment(shclw_doc *d, const char *path, const char *text, size_t len, shclw_error *err);
// Takes out every instance of path and what's under it, and says how many.
size_t shclw_remove(shclw_doc *d, const char *path);

// The shcl document underneath. On 3.0 a shclw read gives shcl's read memory
// back, so a result from shcl's own read calls on it is good only until the
// next shclw call. On a layered document it's the merge, built again after
// each edit, so a change made on it is gone after the next one, and never
// saved.
shcl_doc *shclw_inner(shclw_doc *d);

// A whole file, which has to be UTF-8. A missing one is SHCLW_ERR_NOT_FOUND.
// Caller frees *text.
shclw_kind shclw_read_text(const char *path, char **text, size_t *len, shclw_error *err);

// Runs an ops script on a document parsed from input, and gives back what the
// ops printed (project/design.md, "Ops scripts"). dir is where `check` and
// `write` may touch files; NULL turns them off. Caller frees. NULL only when
// memory ran out.
char *shclw_run(const char *input, size_t len, const char *script, size_t script_len, const char *dir, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
