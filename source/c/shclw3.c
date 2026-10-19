// Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
// SPDX-License-Identifier: MIT

// The wrapper over shcl 3.0.
//
// Documents keep their lines as typed, so shclw_text and shclw_write give back
// what was read plus the edits, where 3.0 can manage that, and the canonical
// form where it can't. shclw_rewrites says which. shclw_write refuses with
// SHCLW_ERR_REFUSED rather than drop a line.
//
// Every read copies its answer out and then gives back shcl's read memory, so
// a program that reads in a loop doesn't grow (2026101513330046).

#include "shclw3_v2.h"
#include "shclw_adapter.h"

#if SHCLW_FORMAT_MAJOR != 3
#error "shclw3.c is the adapter for shcl 3.0, and this shcl.h is another version"
#endif

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The file shclw_open upgraded, kept until the first shclw_write over it backs
// it up. text is what the file held, and backup the copy's name once made.
typedef struct {
	int64_t from;
	char *path;
	shclw_i_file_id id;
	char *text;
	size_t len;
	char *backup;
} upgraded_file;

struct shclw_doc {
	shcl_doc *d;
	shclw_i_info info;
	bool stamp;
	upgraded_file *up;
	// shcl 2.0's errors on a file shclw_open refused, which Go and Rust put on
	// the error.
	shclw_diag *refused;
	size_t n_refused;
};

static void free_upgraded(upgraded_file *up) {
	if (up) {
		free(up->path);
		free(up->text);
		free(up->backup);
		free(up);
	}
}

static shclw_status status_of(shcl_status s) {
	switch (s) {
	case SHCL_GOOD: return SHCLW_GOOD;
	case SHCL_EMPTY: return SHCLW_EMPTY;
	case SHCL_NOT_FOUND: return SHCLW_NOT_FOUND;
	case SHCL_BAD_TYPE: return SHCLW_BAD_TYPE;
	case SHCL_MULTIPLE: return SHCLW_MULTIPLE;
	case SHCL_BAD_PATH: return SHCLW_BAD_PATH;
	}
	return SHCLW_BAD_PATH;
}

static shcl_strictness level_of(shclw_strictness s) {
	switch (s) {
	case SHCLW_LOOSE: return SHCL_LOOSE;
	case SHCLW_STRICT: return SHCL_STRICT;
	case SHCLW_STANDARD: return SHCL_STANDARD;
	}
	return SHCL_STANDARD;
}

int64_t shclw_format_of(const char *text, size_t len) {
	shcl_read_i64 r = shcl_read_format_version(text ? text : "", text ? len : 0);
	return r.status == SHCL_GOOD ? r.value : 0;
}

static shclw_kind too_new(shclw_error *err, const char *path, int64_t format) {
	char why[128];
	(void)snprintf(why, sizeof why, "format %" PRId64 ", and shcl %s reads up to %d", format, SHCLW_SHCL_VERSION,
	               SHCLW_FORMAT_MAJOR);
	return shclw_i_fail(err, SHCLW_ERR_TOO_NEW, path, why, 0);
}

shclw_doc *shclw_new(void) { return shclw_parse("", 0); }

shclw_doc *shclw_parse(const char *text, size_t len) {
	shclw_doc *d = NULL;
	shclw_parse_with(text, len, SHCLW_STANDARD, &d, NULL);
	return d;
}

shclw_kind shclw_parse_with(const char *text, size_t len, shclw_strictness level, shclw_doc **doc, shclw_error *err) {
	*doc = NULL;
	shclw_doc *d = (shclw_doc *)malloc(sizeof *d);
	if (!d) {
		return shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, NULL, NULL, 0);
	}
	d->d = shcl_parse_keep_lines(text ? text : "", text ? len : 0, level_of(level));
	d->info = SHCLW_I_INFO_AS_IS;
	d->stamp = false;
	d->up = NULL;
	d->refused = NULL;
	d->n_refused = 0;
	if (!d->d) {
		free(d);
		return shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, NULL, NULL, 0);
	}
	*doc = d;
	if (shcl_strict_failed(d->d)) {
		char why[64];
		(void)snprintf(why, sizeof why, "the load has %zu errors", shcl_error_count(d->d));
		return shclw_i_fail(err, SHCLW_ERR_STRICT, NULL, why, 0);
	}
	return shclw_i_ok(err);
}

static shclw_kind upgrade(const char *text, size_t len, int64_t from, const char *path, shclw_upgraded **out,
                          shclw_error *err);

shclw_kind shclw_open(const char *path, shclw_doc **doc, shclw_error *err) {
	return shclw_open_with(path, SHCLW_STANDARD, doc, err);
}

shclw_kind shclw_open_with(const char *path, shclw_strictness level, shclw_doc **doc, shclw_error *err) {
	*doc = NULL;
	char *text = NULL;
	size_t len = 0;
	shclw_kind kind = shclw_read_text(path, &text, &len, err);
	if (kind == SHCLW_OK) {
		int64_t format = shclw_format_of(text, len);
		if (format > SHCLW_FORMAT_MAJOR) {
			kind = too_new(err, path, format);
		}
	}
	// A file naming this format is current, so it skips the upgrade's parse.
	shclw_upgraded *u = NULL;
	if (kind == SHCLW_OK && shclw_format_of(text, len) != SHCLW_FORMAT_MAJOR) {
		kind = upgrade(text, len, 0, path, &u, err);
	}
	if (kind != SHCLW_OK) {
		free(text);
		*doc = shclw_new();
		if (*doc && u && u->n_diags) {
			(*doc)->refused = shclw_i_copy_diags(u->diags, u->n_diags, &(*doc)->n_refused);
		}
		free(u);
		return *doc ? kind : shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
	}
	upgraded_file *up = NULL;
	if (u && u->changed) {
		up = (upgraded_file *)malloc(sizeof *up);
		char *name = shclw_i_dup(path, strlen(path), NULL);
		if (!up || !name) {
			free(up);
			free(name);
			free(u);
			free(text);
			return shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
		}
		up->from = u->from;
		up->path = name;
		up->id = shclw_i_file_id_of(path);
		up->text = text;
		up->len = len;
		up->backup = NULL;
		kind = shclw_parse_with(u->text.p, u->text.n, level, doc, NULL);
	} else {
		kind = shclw_parse_with(text, len, level, doc, NULL);
		free(text);
	}
	free(u);
	if (*doc) {
		(*doc)->up = up;
	} else {
		free_upgraded(up);
	}
	switch (kind) {
	case SHCLW_OK: return shclw_i_ok(err);
	case SHCLW_ERR_STRICT: {
		char why[64];
		(void)snprintf(why, sizeof why, "the load has %zu errors", *doc ? shcl_error_count((*doc)->d) : 0);
		return shclw_i_fail(err, kind, path, why, 0);
	}
	default: return shclw_i_fail(err, kind, path, NULL, 0);
	}
}

void shclw_free(shclw_doc *d) {
	if (d) {
		shcl_free(d->d);
		free_upgraded(d->up);
		free(d->refused);
		free(d);
	}
}

int64_t shclw_upgraded_from(shclw_doc *d) { return d->up ? d->up->from : 0; }

char *shclw_backup_name(shclw_doc *d, size_t *len) {
	const char *name = d->up && d->up->backup ? d->up->backup : "";
	return shclw_i_dup(name, strlen(name), len);
}

shcl_doc *shclw_inner(shclw_doc *d) { return d->d; }

void shclw_compact(shclw_doc *d) {
	if (d) {
		shcl_compact(d->d);
	}
}

// Every field's path and value and the load's error codes, for
// shclw_i_finish_info to tell a comment from a raw body's line.
static char *values(const char *text, size_t len, size_t *n) {
	*n = 0;
	shcl_doc *d = shcl_parse(text, len);
	if (!d) {
		return NULL;
	}
	shcl_field *fields = NULL;
	size_t count = shcl_fields(d, &fields);
	size_t total = 0;
	bool big = false;
	for (size_t i = 0; i < count && !big; i++) {
		size_t more = fields[i].path.n + fields[i].value.n + 2;
		big = fields[i].path.n > SIZE_MAX / 8 || fields[i].value.n > SIZE_MAX / 8 || more > SIZE_MAX / 4 - total;
		total += more;
	}
	size_t ndiags = shcl_diag_count(d);
	for (size_t i = 0; i < ndiags && !big; i++) {
		size_t more = strlen(shcl_diag_code(d, i)) + 1;
		big = more > SIZE_MAX / 4 - total;
		total += more;
	}
	char *out = big ? NULL : (char *)malloc(total + 1);
	if (out) {
		char *at = out;
		for (size_t i = 0; i < count; i++) {
			if (fields[i].path.n) {
				memcpy(at, fields[i].path.p, fields[i].path.n);
			}
			at += fields[i].path.n;
			*at++ = '\0';
			if (fields[i].value.n) {
				memcpy(at, fields[i].value.p, fields[i].value.n);
			}
			at += fields[i].value.n;
			*at++ = '\1';
		}
		for (size_t i = 0; i < ndiags; i++) {
			if (shcl_diag_severity(d, i) == SHCL_SEV_ERROR) {
				const char *code = shcl_diag_code(d, i);
				size_t code_len = strlen(code);
				memcpy(at, code, code_len);
				at += code_len;
				*at++ = '\2';
			}
		}
		*n = (size_t)(at - out);
	}
	shcl_free(d);
	return out;
}

static const shclw_i_info_edit info_edit = {SHCL_GEN_BANNER, SHCL_FORMAT_MAJOR, values};

// s with the info block and the Format line as asked, or s as it is when an
// edit since made them impossible; shclw_write says why.
static char *finish(shclw_doc *d, shcl_str s, size_t *len) {
	const char *text = s.p ? s.p : "";
	size_t text_len = s.p ? s.n : 0;
	char *out = NULL;
	size_t n = 0;
	shclw_kind k = shclw_i_finish_info(text, text_len, d->info, d->stamp, &info_edit, NULL, &out, &n, NULL);
	if (k != SHCLW_OK && k != SHCLW_ERR_NO_MEMORY) {
		out = shclw_i_dup(text, text_len, &n);
	}
	shcl_reads_release(d->d);
	if (len) {
		*len = out ? n : 0;
	}
	return out;
}

char *shclw_text(shclw_doc *d, size_t *len) {
	// Without the lines kept, this is the canonical form.
	return finish(d, shcl_to_text_keep_lines(d->d, NULL), len);
}

char *shclw_canonical(shclw_doc *d, size_t *len) { return finish(d, shcl_to_canonical(d->d), len); }

bool shclw_rewrites(shclw_doc *d) {
	if (d->up && !d->up->backup) {
		return true;
	}
	int kept = 0;
	(void)shcl_to_text_keep_lines(d->d, &kept);
	shcl_reads_release(d->d);
	return !kept;
}

// What the save would write with info and stamp, thrown away: InfoBlock and
// StampFormat refuse what it refuses.
static shclw_kind try_info(shclw_doc *d, shclw_i_info info, bool stamp, shclw_error *err) {
	shcl_str s = shcl_to_text_keep_lines(d->d, NULL);
	char *out = NULL;
	size_t n = 0;
	shclw_kind k = shclw_i_finish_info(s.p ? s.p : "", s.n, info, stamp, &info_edit, NULL, &out, &n, err);
	free(out);
	shcl_reads_release(d->d);
	if (k == SHCLW_OK) {
		d->info = info;
		d->stamp = stamp;
	}
	return k;
}

shclw_kind shclw_info_block(shclw_doc *d, bool on, shclw_error *err) {
	return try_info(d, on ? SHCLW_I_INFO_ON : SHCLW_I_INFO_OFF, d->stamp, err);
}

shclw_kind shclw_stamp_format(shclw_doc *d, shclw_error *err) { return try_info(d, d->info, true, err); }

typedef struct {
	const char *path;
	const char *text;
	size_t len;
	int64_t from;
} replacing;

static shclw_kind keep_original(void *ctx, char **name, shclw_error *err) {
	const replacing *r = (const replacing *)ctx;
	char *message = NULL;
	shcl_write_status why = SHCL_WRITE_OK;
	errno = 0;
	shcl_upgrade_error ue = shcl_write_backup(r->path, r->text, r->len, (uint32_t)r->from, name, &message, &why);
	int e = errno;
	if (ue == SHCL_UPGRADE_OK) {
		free(message);
		return shclw_i_ok(err);
	}
	shclw_kind k = shclw_i_fail(err, ue == SHCL_UPGRADE_BACKUP_TAKEN ? SHCLW_ERR_BACKUP_TAKEN : SHCLW_ERR_IO, r->path,
	                            message ? message : shcl_write_status_name(why), ue == SHCL_UPGRADE_IO ? e : 0);
	free(message);
	free(*name);
	*name = NULL;
	return k;
}

static shclw_kind write_new(void *ctx, const char *path, const char *data, size_t len, shclw_error *err) {
	(void)ctx;
	shcl_write_status why = SHCL_WRITE_OK;
	errno = 0;
	if (shcl_write_file_atomic(path, data, len, &why)) {
		return shclw_i_ok(err);
	}
	return shclw_i_fail(err, SHCLW_ERR_IO, path, shcl_write_status_name(why), errno);
}

// shcl's atomic save, with a missing folder as SHCLW_ERR_NOT_FOUND.
static shclw_kind save(void *ctx, const char *path, const char *data, size_t len, shclw_error *err) {
	(void)ctx;
	shcl_write_status why = SHCL_WRITE_OK;
	errno = 0;
	if (shcl_write_file_atomic(path, data, len, &why)) {
		return shclw_i_ok(err);
	}
	int e = errno;
	shclw_kind kind = why == SHCL_WRITE_NOT_FOUND ? SHCLW_ERR_NOT_FOUND : SHCLW_ERR_IO;
	return shclw_i_fail(err, kind, path, shcl_write_status_name(why), e);
}

shclw_kind shclw_write(shclw_doc *d, const char *path, shclw_error *err) {
	int kept = 0;
	shcl_str s = shcl_to_text_keep_lines(d->d, &kept);
	size_t lost = shcl_lost_count(d->d);
	if (!kept && lost > 0) {
		shcl_reads_release(d->d);
		char why[64];
		(void)snprintf(why, sizeof why, "%zu lines would be lost", lost);
		return shclw_i_fail(err, SHCLW_ERR_REFUSED, path, why, 0);
	}
	char *text = NULL;
	size_t len = 0;
	shclw_kind k = shclw_i_finish_info(s.p ? s.p : "", s.n, d->info, d->stamp, &info_edit, path, &text, &len, err);
	shcl_reads_release(d->d);
	if (k != SHCLW_OK) {
		return k;
	}
	upgraded_file *up = d->up;
	if (up && !up->backup && shclw_i_same_file(path, up->path, up->id)) {
		// The first save over a file shclw_open upgraded keeps the original.
		replacing r = {path, up->text, up->len, up->from};
		shclw_i_replacer how = {keep_original, save, &r};
		k = shclw_i_replace_read(path, up->text, up->len, text, len, &how, &up->backup, err);
		if (k == SHCLW_OK) {
			free(up->text);
			up->text = NULL;
			up->len = 0;
		}
	} else {
		k = save(NULL, path, text, len, err);
	}
	free(text);
	if (k == SHCLW_OK) {
		// A save is where a program is done with a round of edits, and what
		// they replaced stays in shcl's write memory until this
		// (2026101516110996).
		shcl_compact(d->d);
	}
	return k;
}

static shclw_diag diag_of(size_t line, shcl_severity sev, const char *code, shcl_str msg) {
	shclw_diag g = {
		line, sev == SHCL_SEV_ERROR ? SHCLW_SEV_ERROR : SHCLW_SEV_HINT, {code, strlen(code)}, {msg.p, msg.n}};
	return g;
}

shclw_diag *shclw_diags(shclw_doc *d, size_t *n) {
	*n = 0;
	if (d->refused) {
		return shclw_i_copy_diags(d->refused, d->n_refused, n);
	}
	size_t count = shcl_diag_count(d->d);
	shclw_diag *views =
		count < SIZE_MAX / sizeof(shclw_diag) ? (shclw_diag *)shclw_i_alloc(count * sizeof(shclw_diag)) : NULL;
	if (!views) {
		return NULL;
	}
	for (size_t i = 0; i < count; i++) {
		views[i] = diag_of(shcl_diag_line(d->d, i), shcl_diag_severity(d->d, i), shcl_diag_code(d->d, i),
		                   shcl_diag_message(d->d, i));
	}
	shclw_diag *out = shclw_i_copy_diags(views, count, n);
	free(views);
	return out;
}

size_t shclw_errors(shclw_doc *d) { return d->refused ? d->n_refused : shcl_error_count(d->d); }

shclw_diag *shclw_check(shclw_doc *d, shclw_doc *schema, size_t *n) {
	*n = 0;
	shcl_validation *v = shcl_validate(d->d, schema->d);
	if (!v) {
		return NULL;
	}
	size_t count = shcl_validation_count(v);
	shclw_diag *views =
		count < SIZE_MAX / sizeof(shclw_diag) ? (shclw_diag *)shclw_i_alloc(count * sizeof(shclw_diag)) : NULL;
	shclw_diag *out = NULL;
	if (views) {
		for (size_t i = 0; i < count; i++) {
			views[i] = diag_of(shcl_validation_line(v, i), shcl_validation_severity(v, i), shcl_validation_code(v, i),
			                   shcl_validation_message(v, i));
		}
		out = shclw_i_copy_diags(views, count, n);
		free(views);
	}
	shcl_validation_free(v);
	shcl_reads_release(d->d);
	shcl_reads_release(schema->d);
	return out;
}

void shclw_drop_declared(shclw_doc *schema, shclw_doc *d) {
	shcl_suppress_declared_repeats(schema->d, d->d);
	shcl_suppress_declared_reopens(schema->d, d->d);
}

int64_t shclw_int(shclw_doc *d, const char *path, int64_t fallback) {
	int64_t v = shcl_get_int_or(d->d, path, strlen(path), fallback);
	shcl_reads_release(d->d);
	return v;
}

// Through shclw_get_float, so infinity gets the fallback too.
double shclw_float(shclw_doc *d, const char *path, double fallback) {
	shclw_got_float g = shclw_get_float(d, path);
	return g.status == SHCLW_GOOD ? g.value : fallback;
}

bool shclw_bool(shclw_doc *d, const char *path, bool fallback) {
	int v = shcl_get_bool_or(d->d, path, strlen(path), fallback ? 1 : 0);
	shcl_reads_release(d->d);
	return v != 0;
}

char *shclw_str(shclw_doc *d, const char *path, const char *fallback, size_t fallback_len, size_t *len) {
	shclw_got_str g = shclw_get_str(d, path);
	if (g.status == SHCLW_GOOD) {
		if (len) {
			*len = g.len;
		}
		return g.value;
	}
	free(g.value);
	return shclw_i_dup(fallback, fallback ? fallback_len : 0, len);
}

char *shclw_raw(shclw_doc *d, const char *path, const char *fallback, size_t fallback_len, size_t *len) {
	shclw_got_str g = shclw_get_raw(d, path);
	if (g.status == SHCLW_GOOD) {
		if (len) {
			*len = g.len;
		}
		return g.value;
	}
	free(g.value);
	return shclw_i_dup(fallback, fallback ? fallback_len : 0, len);
}

shclw_string *shclw_strs(shclw_doc *d, const char *path, const shclw_string *fallback, size_t fallback_n, size_t *n) {
	shclw_got_strs g = shclw_get_strs(d, path);
	if (g.status == SHCLW_GOOD) {
		*n = g.n;
		return g.value;
	}
	free(g.value);
	return shclw_i_copy_strs(fallback, fallback_n, n);
}

int64_t *shclw_ints(shclw_doc *d, const char *path, const int64_t *fallback, size_t fallback_n, size_t *n) {
	shclw_got_ints g = shclw_get_ints(d, path);
	if (g.status == SHCLW_GOOD) {
		*n = g.n;
		return g.value;
	}
	free(g.value);
	return shclw_i_copy_ints(fallback, fallback_n, n);
}

double *shclw_floats(shclw_doc *d, const char *path, const double *fallback, size_t fallback_n, size_t *n) {
	shclw_got_floats g = shclw_get_floats(d, path);
	if (g.status == SHCLW_GOOD) {
		*n = g.n;
		return g.value;
	}
	free(g.value);
	return shclw_i_copy_floats(fallback, fallback_n, n);
}

// shcl's own reads leave the line out of their result. shcl_line gives it for
// a path that names one node, which is when Go's and Rust's reads have one.
static size_t line_of(shclw_doc *d, const char *path, size_t plen) { return shcl_line(d->d, path, plen); }

shclw_got_int shclw_get_int(shclw_doc *d, const char *path) {
	size_t plen = strlen(path);
	shcl_read_i64 r = shcl_read_int(d->d, path, plen);
	shclw_got_int g = {r.value, status_of(r.status), line_of(d, path, plen)};
	shcl_reads_release(d->d);
	return g;
}

shclw_got_float shclw_get_float(shclw_doc *d, const char *path) {
	size_t plen = strlen(path);
	shcl_read_f64 r = shcl_read_float(d->d, path, plen);
	shclw_got_float g = {r.value, status_of(r.status), line_of(d, path, plen)};
	shcl_reads_release(d->d);
	shclw_i_finite(&g);
	return g;
}

shclw_got_bool shclw_get_bool(shclw_doc *d, const char *path) {
	size_t plen = strlen(path);
	shcl_read_bool r = shcl_read_bool_(d->d, path, plen);
	shclw_got_bool g = {r.value != 0, status_of(r.status), line_of(d, path, plen)};
	shcl_reads_release(d->d);
	return g;
}

static shclw_got_str got_str(shclw_doc *d, const char *path, size_t plen, shcl_read_str r) {
	shclw_got_str g = {NULL, 0, status_of(r.status), line_of(d, path, plen)};
	g.value = shclw_i_dup(r.value.p, r.value.n, &g.len);
	shcl_reads_release(d->d);
	return g;
}

shclw_got_str shclw_get_str(shclw_doc *d, const char *path) {
	size_t plen = strlen(path);
	return got_str(d, path, plen, shcl_read_string(d->d, path, plen));
}

shclw_got_str shclw_get_raw(shclw_doc *d, const char *path) {
	size_t plen = strlen(path);
	return got_str(d, path, plen, shcl_read_raw(d->d, path, plen));
}

shclw_got_strs shclw_get_strs(shclw_doc *d, const char *path) {
	size_t plen = strlen(path);
	shcl_read_str_arr r = shcl_read_string_array(d->d, path, plen);
	shclw_got_strs g = {NULL, 0, status_of(r.status), line_of(d, path, plen)};
	size_t n = r.values ? r.n : 0;
	shclw_string *items = (shclw_string *)shclw_i_alloc(n * sizeof(shclw_string));
	if (items) {
		for (size_t i = 0; i < n; i++) {
			items[i].p = r.values[i].p;
			items[i].n = r.values[i].n;
		}
		g.value = shclw_i_copy_strs(items, n, &g.n);
		free(items);
	}
	shcl_reads_release(d->d);
	return g;
}

shclw_got_ints shclw_get_ints(shclw_doc *d, const char *path) {
	size_t plen = strlen(path);
	shcl_read_i64_arr r = shcl_read_int_array(d->d, path, plen);
	shclw_got_ints g = {NULL, 0, status_of(r.status), line_of(d, path, plen)};
	g.value = shclw_i_copy_ints(r.values, r.values ? r.n : 0, &g.n);
	shcl_reads_release(d->d);
	return g;
}

shclw_got_floats shclw_get_floats(shclw_doc *d, const char *path) {
	size_t plen = strlen(path);
	shcl_read_f64_arr r = shcl_read_float_array(d->d, path, plen);
	shclw_got_floats g = {NULL, 0, status_of(r.status), line_of(d, path, plen)};
	g.value = shclw_i_copy_floats(r.values, r.values ? r.n : 0, &g.n);
	shcl_reads_release(d->d);
	shclw_i_finite_all(&g);
	return g;
}

bool shclw_has(shclw_doc *d, const char *path) {
	int v = shcl_exists(d->d, path, strlen(path));
	shcl_reads_release(d->d);
	return v != 0;
}

size_t shclw_count(shclw_doc *d, const char *path) {
	size_t n = shcl_count(d->d, path, strlen(path));
	shcl_reads_release(d->d);
	return n;
}

size_t shclw_line(shclw_doc *d, const char *path) {
	size_t n = shcl_line(d->d, path, strlen(path));
	shcl_reads_release(d->d);
	return n;
}

// 3.0 lists every instance's children, a name once per line it's on.
shclw_string *shclw_names(shclw_doc *d, const char *path, size_t *n) {
	*n = 0;
	shcl_str *kids = NULL;
	size_t count = shcl_children(d->d, path, strlen(path), &kids);
	shclw_string *names = (shclw_string *)shclw_i_alloc(count * sizeof(shclw_string));
	shclw_string *out = NULL;
	if (names) {
		for (size_t i = 0; i < count; i++) {
			names[i].p = kids[i].p;
			names[i].n = kids[i].n;
		}
		out = shclw_i_dedupe_names(names, count, n);
		free(names);
	}
	shcl_reads_release(d->d);
	return out;
}

// 3.0's own instance paths, with an index on every step that repeats. A
// wildcard slot that reaches no node or several has none, and also is left out.
shclw_node *shclw_each(shclw_doc *d, const char *path, size_t *n) {
	*n = 0;
	shcl_read_field_list r = shcl_read_fields(d->d, path, strlen(path));
	size_t count = r.values ? r.n : 0;
	shclw_node *nodes = (shclw_node *)shclw_i_alloc(count * sizeof(shclw_node));
	shclw_node *out = NULL;
	if (nodes) {
		size_t kept = 0;
		for (size_t i = 0; i < count; i++) {
			if (!r.values[i].path.n) {
				continue;
			}
			nodes[kept].path.p = r.values[i].path.p;
			nodes[kept].path.n = r.values[i].path.n;
			nodes[kept].value.p = r.values[i].value.p;
			nodes[kept].value.n = r.values[i].value.n;
			nodes[kept].line = r.values[i].line;
			kept++;
		}
		out = shclw_i_copy_nodes(nodes, kept, n);
		free(nodes);
	}
	shcl_reads_release(d->d);
	return out;
}

char *shclw_child(shclw_doc *d, const char *base, const char *name, size_t name_len, size_t *len) {
	if (len) {
		*len = 0;
	}
	shcl_str seg = shcl_quote_segment(d->d, name ? name : "", name ? name_len : 0);
	size_t blen = strlen(base);
	size_t total = blen ? blen + 1 + seg.n : seg.n;
	char *out = seg.n < SIZE_MAX / 2 && blen < SIZE_MAX / 4 ? (char *)malloc(total + 1) : NULL;
	if (out) {
		if (blen) {
			memcpy(out, base, blen);
			out[blen] = '.';
		}
		memcpy(out + total - seg.n, seg.p, seg.n);
		out[total] = '\0';
		if (len) {
			*len = total;
		}
	}
	shcl_reads_release(d->d);
	return out;
}

static shclw_kind set_err(shclw_error *err, const char *path, shcl_set_status st) {
	switch (st) {
	case SHCL_SET_OK: return shclw_i_ok(err);
	case SHCL_SET_BAD_PATH:
	case SHCL_SET_VALUE_IN_PATH:
	case SHCL_SET_WILDCARD:
	case SHCL_SET_NO_SUCH_INDEX:
	case SHCL_SET_TOO_DEEP:
	case SHCL_SET_MULTIPLE:
	case SHCL_SET_UNDER_ARRAY:
	case SHCL_SET_HAS_CHILDREN:
	case SHCL_SET_RAW_BLOCK: return shclw_i_fail(err, SHCLW_ERR_BAD_PATH, path, shcl_set_status_name(st), 0);
	default: return shclw_i_fail(err, SHCLW_ERR_BAD_VALUE, path, shcl_set_status_name(st), 0);
	}
}

static shclw_kind set_strs(shclw_doc *d, const char *path, size_t plen, const shclw_i_value *v, shclw_error *err) {
	const char **items = (const char **)shclw_i_alloc(v->n * sizeof(char *));
	size_t *lens = (size_t *)shclw_i_alloc(v->n * sizeof(size_t));
	if (!items || !lens || v->n > SIZE_MAX / sizeof(char *)) {
		free((void *)items);
		free(lens);
		return shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
	}
	for (size_t i = 0; i < v->n; i++) {
		items[i] = v->strs[i].p ? v->strs[i].p : "";
		lens[i] = v->strs[i].p ? v->strs[i].n : 0;
	}
	shcl_set_status st = shcl_set_string_array(d->d, path, plen, items, lens, v->n);
	free((void *)items);
	free(lens);
	return set_err(err, path, st);
}

shclw_kind shclw_i_set(shclw_doc *d, const char *path, const shclw_i_value *v, shclw_error *err) {
	size_t plen = strlen(path);
	shcl_set_status st = SHCL_SET_OK;
	switch (v->kind) {
	case SHCLW_I_INT: st = shcl_set_int(d->d, path, plen, v->i); break;
	case SHCLW_I_FLOAT: st = shcl_set_float(d->d, path, plen, v->f); break;
	case SHCLW_I_BOOL: st = shcl_set_bool(d->d, path, plen, v->b ? 1 : 0); break;
	case SHCLW_I_STR: st = shcl_set_string(d->d, path, plen, v->str, v->n); break;
	case SHCLW_I_STRS: return set_strs(d, path, plen, v, err);
	case SHCLW_I_INTS: st = shcl_set_int_array(d->d, path, plen, v->ints, v->n); break;
	case SHCLW_I_FLOATS: st = shcl_set_float_array(d->d, path, plen, v->floats, v->n); break;
	}
	return set_err(err, path, st);
}

shclw_kind shclw_put_syntax(shclw_doc *d, const char *path, const char *text, size_t len, shclw_error *err) {
	if (!text) {
		text = "";
		len = 0;
	}
	shclw_kind k = set_err(err, path, shcl_set_literal(d->d, path, strlen(path), text, len));
	if (k != SHCLW_OK) {
		return k;
	}
	if (len > SIZE_MAX - 8) {
		return shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
	}
	static const char key[] = "k: ";
	size_t klen = sizeof key - 1;
	char *typed = (char *)malloc(klen + len + 1);
	shclw_doc *alone = shclw_new();
	shclw_doc *line = NULL;
	if (typed) {
		memcpy(typed, key, klen);
		if (len) {
			memcpy(typed + klen, text, len);
		}
		typed[klen + len] = '\n';
		line = shclw_parse(typed, klen + len + 1);
		free(typed);
	}
	if (!alone || !line) {
		shclw_free(alone);
		shclw_free(line);
		return shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
	}
	// A failed set leaves alone empty, which the check reports.
	(void)shcl_set_literal(alone->d, "k", 1, text, len);
	k = shclw_i_verify_syntax(d, path, alone, line, err);
	shclw_free(alone);
	shclw_free(line);
	return k;
}

shclw_kind shclw_comment(shclw_doc *d, const char *path, const char *text, size_t len, shclw_error *err) {
	return set_err(err, path, shcl_set_comment(d->d, path, strlen(path), text ? text : "", text ? len : 0));
}

size_t shclw_remove(shclw_doc *d, const char *path) { return shcl_remove(d->d, path, strlen(path)); }

// shcl 2.0's errors, copied out of its document as they come.
typedef struct {
	shclw_diag *v;
	size_t n, cap;
	bool oom;
} old_list;

static void keep_old(void *ctx, size_t line, const char *code, const char *msg, size_t msg_len) {
	old_list *l = (old_list *)ctx;
	if (l->oom) {
		return;
	}
	if (l->n == l->cap) {
		size_t cap = l->cap ? l->cap * 2 : 8;
		shclw_diag *grown =
			cap < SIZE_MAX / sizeof(shclw_diag) ? (shclw_diag *)realloc(l->v, cap * sizeof(shclw_diag)) : NULL;
		if (!grown) {
			l->oom = true;
			return;
		}
		l->v = grown;
		l->cap = cap;
	}
	size_t code_len = strlen(code);
	char *c = shclw_i_dup(code, code_len, NULL);
	char *m = shclw_i_dup(msg, msg_len, NULL);
	if (!c || !m) {
		free(c);
		free(m);
		l->oom = true;
		return;
	}
	shclw_diag g = {line, SHCLW_SEV_ERROR, {c, code_len}, {m, msg_len}};
	l->v[l->n++] = g;
}

static void free_old(old_list *l) {
	for (size_t i = 0; i < l->n; i++) {
		free((void *)l->v[i].code.p);
		free((void *)l->v[i].message.p);
	}
	free(l->v);
}

static int by_size(const void *a, const void *b) {
	size_t x = *(const size_t *)a, y = *(const size_t *)b;
	return x < y ? -1 : x > y;
}

// How many lines the errors are on.
static size_t lines_in(const old_list *l) {
	size_t *lines = (size_t *)shclw_i_alloc(l->n * sizeof(size_t));
	if (!lines) {
		return l->n;
	}
	for (size_t i = 0; i < l->n; i++) {
		lines[i] = l->v[i].line;
	}
	qsort(lines, l->n, sizeof(size_t), by_size);
	size_t count = 0;
	for (size_t i = 0; i < l->n; i++) {
		count += i == 0 || lines[i] != lines[i - 1];
	}
	free(lines);
	return count;
}

// Go's Upgrade, with path for the error when a file is behind it.
static shclw_kind upgrade(const char *text, size_t len, int64_t from, const char *path, shclw_upgraded **out,
                          shclw_error *err) {
	*out = NULL;
	if (!text) {
		text = "";
		len = 0;
	}
	int64_t named = shclw_format_of(text, len);
	int64_t f = named > from ? named : from;
	if (f > SHCLW_FORMAT_MAJOR) {
		*out = shclw_i_upgraded(text, len, f, false, 0, NULL, NULL, 0);
		return *out ? too_new(err, path, f) : shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
	}
	bool from_v2 = from > 0 && from < SHCLW_FORMAT_MAJOR;
	shcl_upgraded u = shcl_upgrade(text, len, from_v2 ? 1 : 0);
	if (!u.text) {
		shcl_upgraded_free(&u);
		return shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
	}
	int64_t was = u.format;
	// shcl says 2 for a file with no Format line, even one it leaves alone.
	if (u.current && !from_v2 && named == 0) {
		was = SHCLW_FORMAT_MAJOR;
	}
	shclw_kind k = SHCLW_OK;
	if (u.ambiguous > 0) {
		*out = shclw_i_upgraded(u.text, u.len, was, !u.current, u.lost, NULL, NULL, 0);
		char why[64];
		(void)snprintf(why, sizeof why, "%zu lines read two ways", u.ambiguous);
		k = shclw_i_fail(err, SHCLW_ERR_AMBIGUOUS, path, why, 0);
	} else if (u.current && !(from_v2 && u.format < SHCLW_FORMAT_MAJOR)) {
		*out = shclw_i_upgraded(u.text, u.len, was, false, u.lost, NULL, NULL, 0);
	} else {
		// A file taken as 2.x that 2.0 reports errors on. The old program
		// read those lines as it repaired them, or not at all, and an open raw
		// block took the rest of the file. The upgrade would turn that into a
		// clean file that means something else (design.md, Format and upgrade).
		old_list l = {NULL, 0, 0, false};
		(void)shclw_i_v2_errors(text, len, keep_old, &l);
		if (l.oom) {
			k = shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
		} else if (l.n) {
			*out = shclw_i_upgraded(text, len, u.format, false, 0, NULL, l.v, l.n);
			char why[96];
			(void)snprintf(why, sizeof why, "shcl 2.0 reports errors on %zu lines; fix them and upgrade again",
			               lines_in(&l));
			k = shclw_i_fail(err, SHCLW_ERR_REFUSED, path, why, 0);
		} else {
			*out = shclw_i_upgraded(u.text, u.len, was, !u.current, u.lost, NULL, NULL, 0);
		}
		free_old(&l);
	}
	shcl_upgraded_free(&u);
	if (!*out) {
		return shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
	}
	return k == SHCLW_OK ? shclw_i_ok(err) : k;
}

shclw_kind shclw_upgrade(const char *text, size_t len, int64_t from, shclw_upgraded **out, shclw_error *err) {
	return upgrade(text, len, from, NULL, out, err);
}

// The result of a file upgrade that wrote nothing: only the format, and 2.0's
// errors when that's why.
static shclw_kind none(shclw_kind k, int64_t from, const shclw_upgraded *u, shclw_upgraded **out, shclw_error *err) {
	*out = shclw_i_upgraded("", 0, from, false, 0, NULL, u ? u->diags : NULL, u ? u->n_diags : 0);
	return *out ? k : shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, NULL, NULL, 0);
}

shclw_kind shclw_upgrade_file(const char *path, int64_t from, shclw_upgraded **out, shclw_error *err) {
	*out = NULL;
	if (from > SHCLW_FORMAT_MAJOR) {
		return none(too_new(err, path, from), from, NULL, out, err);
	}
	// A FIFO would block the read, and the write would put a file in place of
	// a device.
	if (shclw_i_irregular(path)) {
		return none(shclw_i_fail(err, SHCLW_ERR_IO, path, "not a regular file", 0), 0, NULL, out, err);
	}
	// One read for the 2.0 check, the conversion, and the backup. shcl's own
	// shcl_upgrade_file would read the file a second time, after the check.
	char *text = NULL;
	size_t len = 0;
	shclw_kind k = shclw_read_text(path, &text, &len, err);
	if (k != SHCLW_OK) {
		return none(k, 0, NULL, out, err);
	}
	shclw_upgraded *u = NULL;
	k = upgrade(text, len, from, path, &u, err);
	if (!u) {
		free(text);
		return k;
	}
	if (k != SHCLW_OK || !u->changed) {
		free(text);
		if (k == SHCLW_OK) {
			*out = u;
			return k;
		}
		k = none(k, u->from, u, out, err);
		free(u);
		return k;
	}
	replacing r = {path, text, len, u->from};
	shclw_i_replacer how = {keep_original, write_new, &r};
	char *backup = NULL;
	k = shclw_i_replace_read(path, text, len, u->text.p, u->text.n, &how, &backup, err);
	free(text);
	if (k != SHCLW_OK) {
		k = none(k, u->from, NULL, out, err);
	} else {
		*out = shclw_i_upgraded(u->text.p, u->text.n, u->from, true, u->unsure, backup, NULL, 0);
		k = *out ? k : shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
	}
	free(backup);
	free(u);
	return k;
}
