// Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
// SPDX-License-Identifier: MIT

// What the version adapters and run.c share, from shclw.c. Not for programs.

#ifndef SHCLW_ADAPTER_H
#define SHCLW_ADAPTER_H

#include "shclw.h"

#ifdef __cplusplus
extern "C" {
#endif

// Fills *err, when there is one, and returns kind. path and why may be NULL.
shclw_kind shclw_i_fail(shclw_error *err, shclw_kind kind, const char *path, const char *why, int sys_errno);
shclw_kind shclw_i_ok(shclw_error *err);

// malloc of at least 1 byte, so NULL always means out of memory.
void *shclw_i_alloc(size_t n);
// A copy of p[0..n) with a NUL after it. p may be NULL when n is 0.
char *shclw_i_dup(const char *p, size_t n, size_t *len);
// Copies of the items in one block, each with a NUL after it.
shclw_string *shclw_i_copy_strs(const shclw_string *items, size_t n, size_t *count);
int64_t *shclw_i_copy_ints(const int64_t *items, size_t n, size_t *count);
double *shclw_i_copy_floats(const double *items, size_t n, size_t *count);

// The length of the UTF-8 sequence at s, and its code point in *r, or 0 when
// it isn't a valid one. Go's rules: no overlongs, surrogates, or past U+10FFFF.
size_t shclw_i_utf8_next(const char *s, size_t n, uint32_t *r);
bool shclw_i_utf8_valid(const char *s, size_t n);

// Infinity and NaN read as SHCLW_BAD_TYPE, as in 3.0. An array keeps its
// length, with 0 for each item that wasn't a number.
void shclw_i_finite(shclw_got_float *g);
void shclw_i_finite_all(shclw_got_floats *g);

// A put's value, Go's shclw.Value. n is the length of str or of the array.
typedef enum {
	SHCLW_I_INT,
	SHCLW_I_FLOAT,
	SHCLW_I_BOOL,
	SHCLW_I_STR,
	SHCLW_I_STRS,
	SHCLW_I_INTS,
	SHCLW_I_FLOATS
} shclw_i_kind;
typedef struct {
	shclw_i_kind kind;
	int64_t i;
	double f;
	bool b;
	const char *str;
	const shclw_string *strs;
	const int64_t *ints;
	const double *floats;
	size_t n;
} shclw_i_value;

// A value of kind with every other field 0, and n set.
shclw_i_value shclw_i_value_of(shclw_i_kind kind, size_t n);
// Go's VerifyPut: reads back what a setter wrote, since shcl can take a value
// and read back something else.
shclw_kind shclw_i_verify_put(shclw_doc *d, const char *path, const shclw_i_value *v, shclw_error *err);
// The adapter's half of a put: the set alone, with no read-back. shclw.c
// checks the value first and reads it back after.
shclw_kind shclw_i_set(shclw_doc *d, const char *path, const shclw_i_value *v, shclw_error *err);
// Where edits and saves go: the edit layer of a layered document, or d.
shclw_doc *shclw_i_top(shclw_doc *d);
// Builds a layered document's merge again after an edit to its edit layer. A
// no-op on any other document.
shclw_kind shclw_i_edited(shclw_doc *d, shclw_error *err);
// Go's VerifySyntax. alone has the text set at "k", and line is "k: text"
// parsed.
shclw_kind shclw_i_verify_syntax(shclw_doc *d, const char *path, shclw_doc *alone, shclw_doc *line, shclw_error *err);

// The first of each name, compared without ASCII case, as shcl matches names.
shclw_string *shclw_i_dedupe_names(const shclw_string *names, size_t n, size_t *count);
// Copies of the nodes in one block, each string with a NUL after it.
shclw_node *shclw_i_copy_nodes(const shclw_node *nodes, size_t n, size_t *count);

// The error lines in diags, once each, by layer then line. Caller frees.
shclw_spot *shclw_i_unused_of(const shclw_diag *diags, size_t n, size_t *count);
// One loaded text for shclw_i_merges_of, with the layer its lines are in.
typedef struct {
	const char *text;
	size_t len;
	size_t layer;
} shclw_i_source;
// Go's MergesOf over each source in turn, in one block the caller frees. NULL
// only when memory ran out.
shclw_merge *shclw_i_merges_of(const shclw_i_source *srcs, size_t n_srcs, size_t *count);
// Copies of the diagnostics in one block, each string with a NUL after it.
shclw_diag *shclw_i_copy_diags(const shclw_diag *diags, size_t n, size_t *count);

// An upgrade's result in one block, with copies of text, backup, and diags.
// backup may be NULL for none. NULL only when memory ran out.
shclw_upgraded *shclw_i_upgraded(const char *text, size_t len, int64_t from, bool changed, size_t unsure,
                                 const char *backup, const shclw_diag *diags, size_t n_diags);

// Nothing at path, not even a dangling link.
bool shclw_i_missing(const char *path);

// Something at path that isn't a regular file, such as a FIFO or a device.
bool shclw_i_irregular(const char *path);

// Which file a path names, to tell the same file by another name. ok is false
// when it can't be read, and always on Windows, where only the path counts.
typedef struct {
	bool ok;
	uint64_t dev, ino;
} shclw_i_file_id;
shclw_i_file_id shclw_i_file_id_of(const char *path);
// Whether path names the file was named when it had id: the same path, or
// the same file by another name.
bool shclw_i_same_file(const char *path, const char *was, shclw_i_file_id id);

// What a save does with the info block, Go's shclw.Info.
typedef enum { SHCLW_I_INFO_AS_IS, SHCLW_I_INFO_ON, SHCLW_I_INFO_OFF } shclw_i_info;

// What an adapter hands shclw_i_finish_info. values sums up the values and
// errors text loads to with the bound shcl, malloc'd, or NULL when memory ran
// out. A run of lines whose removal leaves that the same is comments, which is
// how a block's text inside a raw body is told from a real block.
typedef struct {
	const char *block;
	int64_t major;
	char *(*values)(const char *text, size_t len, size_t *n);
} shclw_i_info_edit;

// Go's FinishInfo: text as a save writes it after InfoBlock and StampFormat,
// in *out, which the caller frees. path, which may be NULL, goes in the error.
shclw_kind shclw_i_finish_info(const char *text, size_t len, shclw_i_info info, bool stamp, const shclw_i_info_edit *e,
                               const char *path, char **out, size_t *out_len, shclw_error *err);

// Go's OldBlock: whether text has 2.x's `#` info block as comments, in
// *found, and in *marked text with the first one swapped for a mark
// shclw_i_place_block finds, which the caller frees. *marked is NULL where
// text already has a line like the mark. False when memory ran out.
bool shclw_i_old_block(const char *text, size_t len, const shclw_i_info_edit *e, bool *found, char **marked,
                       size_t *marked_len);

// Go's PlaceBlock: text, an upgrade of the marked text, with every old block
// off and the info block where the mark is, or at the end with no mark, in
// *out, which the caller frees.
shclw_kind shclw_i_place_block(const char *text, size_t len, bool marked, const shclw_i_info_edit *e, const char *path,
                               char **out, size_t *out_len, shclw_error *err);

// Go's SameEnds: text, an upgrade of orig, with orig's line ends where shcl
// wrote it out with LF only. malloc'd, or NULL when memory ran out.
char *shclw_i_same_ends(const char *orig, size_t orig_len, const char *text, size_t len, const shclw_i_info_edit *e,
                        size_t *out_len);

// Go's ReplaceRead: writes data over path, where was is the text last read
// from it. A file that has changed since is left alone, checked before the
// backup and again before the write. backup, when not NULL, keeps was first
// and gives the copy's name, malloc'd, which goes in *backup_name. When the
// write fails, the original stays or is put back from the backup, and then
// the backup goes. Only when neither works is the backup kept, and the error
// names it.
typedef struct {
	shclw_kind (*backup)(void *ctx, char **name, shclw_error *err);
	shclw_kind (*write)(void *ctx, const char *path, const char *data, size_t len, shclw_error *err);
	void *ctx;
} shclw_i_replacer;
shclw_kind shclw_i_replace_read(const char *path, const char *was, size_t was_len, const char *data, size_t len,
                                const shclw_i_replacer *how, char **backup_name, shclw_error *err);

// The shortest digits that read back as f, and the closest of those: Go's
// strconv.FormatFloat(f, 'e', -1, 64), or with single, as a float32. digits
// gets them with no point, at least 40 bytes, and *exp the power of 10 of the
// first. f is finite.
void shclw_i_shortest(double f, bool single, char *digits, int *exp);

#ifdef __cplusplus
}
#endif

#endif
