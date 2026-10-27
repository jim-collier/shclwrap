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

// The file shclw_open read, as it was then, so shclw_write can tell it changed
// since. After a save over it, it's what that save left. from is the format
// shclw_open upgraded it from, or 0, and backup the original's copy once the
// first save over it made one.
typedef struct {
	int64_t from;
	char *path;
	shclw_i_file_id id;
	char *text;
	size_t len;
	char *backup;
	bool gone; // a shclw_open_layers user file that wasn't there
} read_file;

struct shclw_doc {
	shcl_doc *d;
	shclw_i_info info;
	bool stamp;
	read_file *file;
	// The newer format the text names, which makes it read-only.
	int64_t newer;
	// shcl 2.0's errors on a file shclw_open refused, which Go and Rust put on
	// the error.
	shclw_diag *refused;
	size_t n_refused;
	// What shclw_open_layers read, NULL for a file that wasn't there. d is
	// then their merge, built again after each edit, and edits and saves go
	// to layers[edit].
	shclw_doc **layers;
	size_t n_layers, edit;
	// The text as loaded, for shclw_merged.
	char *src;
	size_t src_len;
	// The load's diagnostics, once a fill-in has parsed the text again.
	shclw_diag *loaded;
	size_t n_loaded;
};

static void free_read(read_file *f) {
	if (f) {
		free(f->path);
		free(f->text);
		free(f->backup);
		free(f);
	}
}

// path's record with no text yet, or NULL when memory ran out.
static read_file *new_read(const char *path) {
	read_file *f = (read_file *)calloc(1, sizeof *f);
	char *name = shclw_i_dup(path, strlen(path), NULL);
	if (!f || !name) {
		free(f);
		free(name);
		return NULL;
	}
	f->path = name;
	f->id = shclw_i_file_id_of(path);
	return f;
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
	d->file = NULL;
	d->refused = NULL;
	d->n_refused = 0;
	d->loaded = NULL;
	d->n_loaded = 0;
	d->layers = NULL;
	d->n_layers = 0;
	d->edit = 0;
	d->src = shclw_i_dup(text ? text : "", text ? len : 0, &d->src_len);
	if (!d->d || !d->src) {
		shcl_free(d->d);
		free(d->src);
		free(d);
		return shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, NULL, NULL, 0);
	}
	int64_t format = shclw_format_of(text, text ? len : 0);
	d->newer = format > SHCLW_FORMAT_MAJOR ? format : 0;
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
	// A file naming this format or a newer one skips the upgrade's parse.
	shclw_upgraded *u = NULL;
	if (kind == SHCLW_OK && shclw_format_of(text, len) < SHCLW_FORMAT_MAJOR) {
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
	read_file *f = new_read(path);
	if (!f) {
		free(u);
		free(text);
		return shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
	}
	f->text = text;
	f->len = len;
	if (u && u->changed) {
		f->from = u->from;
		kind = shclw_parse_with(u->text.p, u->text.n, level, doc, NULL);
	} else {
		kind = shclw_parse_with(text, len, level, doc, NULL);
	}
	free(u);
	if (*doc) {
		(*doc)->file = f;
	} else {
		free_read(f);
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

// A layer is never layered itself, so this goes one level down.
static void free_doc(shclw_doc *d) {
	if (d) {
		shcl_free(d->d);
		free_read(d->file);
		free(d->refused);
		free(d->loaded);
		free(d->src);
		free(d);
	}
}

void shclw_free(shclw_doc *d) {
	if (d) {
		for (size_t i = 0; i < d->n_layers; i++) {
			free_doc(d->layers[i]);
		}
		free((void *)d->layers);
		free_doc(d);
	}
}

shclw_doc *shclw_i_top(shclw_doc *d) { return d->layers ? d->layers[d->edit] : d; }

// Lower layers never change, but a merge changes its base, so it starts over.
static shclw_kind remerge(shclw_doc *d, shclw_error *err) {
	shcl_doc *m = shcl_new();
	if (!m) {
		return shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, NULL, NULL, 0);
	}
	for (size_t i = 0; i < d->n_layers; i++) {
		if (d->layers[i]) {
			shcl_merge(m, d->layers[i]->d);
		}
	}
	shcl_free(d->d);
	d->d = m;
	return shclw_i_ok(err);
}

shclw_kind shclw_i_edited(shclw_doc *d, shclw_error *err) { return d->layers ? remerge(d, err) : shclw_i_ok(err); }

shclw_kind shclw_open_layers(const shclw_layer *layers, size_t n, shclw_doc **doc, shclw_error *err) {
	*doc = shclw_new();
	if (!*doc) {
		return shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, NULL, NULL, 0);
	}
	if (n == 0) {
		return shclw_i_ok(err);
	}
	shclw_doc *d = *doc;
	d->layers = (shclw_doc **)calloc(n, sizeof *d->layers);
	if (!d->layers) {
		return shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, NULL, NULL, 0);
	}
	d->n_layers = n;
	d->edit = n - 1;
	for (size_t i = n; i-- > 0;) {
		if (layers[i].file) {
			d->edit = i;
			break;
		}
	}
	for (size_t i = 0; i < n; i++) {
		shclw_doc *x = NULL;
		shclw_kind k = SHCLW_OK;
		if (!layers[i].file) {
			x = shclw_parse(layers[i].text, layers[i].len);
			k = x ? SHCLW_OK : shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, NULL, NULL, 0);
		} else {
			k = shclw_open(layers[i].file, &x, err);
			if (k == SHCLW_ERR_NOT_FOUND) {
				shclw_free(x);
				x = i == d->edit ? shclw_new() : NULL;
				if (x) {
					x->file = new_read(layers[i].file);
					if (x->file) {
						x->file->gone = true;
					} else {
						shclw_free(x);
						x = NULL;
					}
				}
				k = x || i != d->edit ? shclw_i_ok(err) : shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, NULL, NULL, 0);
			}
		}
		if (k != SHCLW_OK) {
			// The empty document shclw_open gave, with 2.0's errors on it.
			shclw_free(d);
			*doc = x ? x : shclw_new();
			for (size_t j = 0; x && j < x->n_refused; j++) {
				x->refused[j].layer = i + 1;
			}
			return *doc ? k : shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, NULL, NULL, 0);
		}
		d->layers[i] = x;
	}
	return remerge(d, err);
}

int64_t shclw_upgraded_from(shclw_doc *d) {
	d = shclw_i_top(d);
	return d->file ? d->file->from : 0;
}

char *shclw_backup_name(shclw_doc *d, size_t *len) {
	d = shclw_i_top(d);
	const char *name = d->file && d->file->backup ? d->file->backup : "";
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

// text as a save writes it, in *out, which the caller frees. Where that has no
// Format line, 2.x's block is swapped for the current one where it sits, as
// the upgrade does, so the next shclw_open doesn't take the file as 2.x
// (2026101513330010).
static shclw_kind saved(const shclw_doc *d, const char *text, size_t len, const char *path, char **out, size_t *out_len,
                        shclw_error *err) {
	shclw_kind k = shclw_i_finish_info(text, len, d->info, d->stamp, &info_edit, path, out, out_len, err);
	if (k != SHCLW_OK || shclw_format_of(*out, *out_len) != 0) {
		return k;
	}
	bool found = false;
	char *marked = NULL;
	size_t marked_len = 0;
	if (!shclw_i_old_block(*out, *out_len, &info_edit, &found, &marked, &marked_len)) {
		k = shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
	} else if (found) {
		char *swapped = NULL;
		size_t swapped_len = 0;
		k = marked ? shclw_i_place_block(marked, marked_len, true, &info_edit, path, &swapped, &swapped_len, err)
		           : shclw_i_place_block(*out, *out_len, false, &info_edit, path, &swapped, &swapped_len, err);
		free(*out);
		*out = swapped;
		*out_len = swapped_len;
	}
	free(marked);
	if (k != SHCLW_OK) {
		free(*out);
		*out = NULL;
		*out_len = 0;
	}
	return k;
}

// s with the info block and the Format line as asked, or s as it is when an
// edit since made them impossible; shclw_write says why.
static char *finish(shclw_doc *d, shcl_str s, size_t *len) {
	const char *text = s.p ? s.p : "";
	size_t text_len = s.p ? s.n : 0;
	char *out = NULL;
	size_t n = 0;
	shclw_kind k = saved(d, text, text_len, NULL, &out, &n, NULL);
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
	d = shclw_i_top(d);
	// Without the lines kept, this is the canonical form.
	return finish(d, shcl_to_text_keep_lines(d->d, NULL), len);
}

char *shclw_canonical(shclw_doc *d, size_t *len) {
	d = shclw_i_top(d);
	return finish(d, shcl_to_canonical(d->d), len);
}

bool shclw_rewrites(shclw_doc *d) {
	d = shclw_i_top(d);
	if (d->file && d->file->from && !d->file->backup) {
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
	d = shclw_i_top(d);
	return try_info(d, on ? SHCLW_I_INFO_ON : SHCLW_I_INFO_OFF, d->stamp, err);
}

shclw_kind shclw_stamp_format(shclw_doc *d, shclw_error *err) {
	d = shclw_i_top(d);
	return try_info(d, d->info, true, err);
}

typedef struct {
	const char *path;
	const char *text;
	size_t len;
	int64_t from;
	unsigned mode; // for a file the save creates
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

// shcl's atomic save, with a missing folder as SHCLW_ERR_NOT_FOUND. ctx is
// NULL or a replacing, whose mode goes on a file the save creates. One that's
// there keeps its own, as shcl's atomic write keeps it.
static shclw_kind save(void *ctx, const char *path, const char *data, size_t len, shclw_error *err) {
	unsigned mode = ctx ? ((const replacing *)ctx)->mode : 0;
	if (mode && shclw_i_missing(path)) {
		shclw_kind k = shclw_i_create_new(path, data, len, mode, err);
		if (k != SHCLW_ERR_EXISTS) {
			return k;
		}
		// made in the meantime
	}
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
	return shclw_write_with(d, path, NULL, err);
}

bool shclw_read_only(shclw_doc *d) { return shclw_i_top(d)->newer > 0; }

// Writes text over the file shclw_open read, unless it changed since. Only the
// moment between the last check and the rename is left open (design.md,
// Loading and saving).
static shclw_kind replace(read_file *f, const char *path, const char *text, size_t len, const shclw_write_opts *o,
                          bool keep_lost, shclw_error *err) {
	// The first save over a file shclw_open upgraded keeps the original, and
	// so does one that would lose lines, with backup.
	bool up_first = f->from && !f->backup;
	replacing r = {path, f->text, f->len, up_first ? f->from : SHCLW_FORMAT_MAJOR, o->mode};
	char *now = NULL;
	size_t now_len = 0;
	const char *was = f->text;
	size_t was_len = f->len;
	if (o->force) {
		if (shclw_read_text(path, &now, &now_len, NULL) != SHCLW_OK) {
			return save(&r, path, text, len, err);
		}
		was = now;
		was_len = now_len;
	} else if (f->gone) {
		if (!shclw_i_missing(path)) {
			return shclw_i_fail(err, SHCLW_ERR_CHANGED, path, "changed since it was read; nothing written", 0);
		}
		return save(&r, path, text, len, err);
	}
	shclw_i_replacer how = {up_first || keep_lost ? keep_original : NULL, save, &r};
	char *name = NULL;
	shclw_kind k = shclw_i_replace_read(path, was, was_len, text, len, &how, &name, err);
	free(now);
	if (name) {
		free(f->backup);
		f->backup = name;
	}
	return k;
}

shclw_kind shclw_write_with(shclw_doc *d, const char *path, const shclw_write_opts *opts, shclw_error *err) {
	d = shclw_i_top(d);
	if (d->newer) {
		return too_new(err, path, d->newer);
	}
	shclw_write_opts o;
	memset(&o, 0, sizeof o);
	if (opts) {
		o = *opts;
	}
	shclw_kind k = shclw_i_check_mode(path, o.mode, err);
	if (k != SHCLW_OK) {
		return k;
	}
	read_file *f = d->file;
	bool over = f && shclw_i_same_file(path, f->path, f->id);
	int kept = 0;
	shcl_str s = shcl_to_text_keep_lines(d->d, &kept);
	size_t lost = shcl_lost_count(d->d);
	bool keep_lost = !kept && lost > 0 && !(f && f->backup);
	if (keep_lost && !(o.backup && over && !o.create && !f->gone)) {
		shcl_reads_release(d->d);
		char why[64];
		(void)snprintf(why, sizeof why, "%zu lines would be lost", lost);
		return shclw_i_fail(err, SHCLW_ERR_REFUSED, path, why, 0);
	}
	char *text = NULL;
	size_t len = 0;
	k = saved(d, s.p ? s.p : "", s.p ? s.n : 0, path, &text, &len, err);
	shcl_reads_release(d->d);
	if (k != SHCLW_OK) {
		return k;
	}
	replacing plain = {path, NULL, 0, 0, o.mode};
	if (o.create) {
		k = shclw_i_create_new(path, text, len, o.mode, err);
	} else {
		k = over ? replace(f, path, text, len, &o, keep_lost, err) : save(&plain, path, text, len, err);
	}
	if (over && k == SHCLW_OK) {
		free(f->text);
		f->text = text;
		f->len = len;
		f->gone = false;
		f->id = shclw_i_file_id_of(path);
	} else {
		free(text);
	}
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
		line, sev == SHCL_SEV_ERROR ? SHCLW_SEV_ERROR : SHCLW_SEV_HINT, {code, strlen(code)}, {msg.p, msg.n}, 0};
	return g;
}

static shclw_diag *own_diags(shclw_doc *d, size_t *n);
static size_t own_errors(shclw_doc *d) {
	if (d->refused) {
		return d->n_refused;
	}
	if (!d->loaded) {
		return shcl_error_count(d->d);
	}
	size_t n = 0;
	for (size_t i = 0; i < d->n_loaded; i++) {
		n += d->loaded[i].severity == SHCLW_SEV_ERROR ? 1 : 0;
	}
	return n;
}

// Every layer's, in layer order, each with its layer.
static shclw_diag *layer_diags(shclw_doc *d, size_t *n) {
	shclw_diag **each = (shclw_diag **)calloc(d->n_layers, sizeof(shclw_diag *));
	size_t *counts = (size_t *)calloc(d->n_layers, sizeof(size_t));
	size_t total = 0;
	bool ok = each && counts;
	for (size_t i = 0; ok && i < d->n_layers; i++) {
		if (d->layers[i]) {
			each[i] = own_diags(d->layers[i], &counts[i]);
			ok = each[i] != NULL;
			total += counts[i];
		}
	}
	shclw_diag *views =
		ok && total < SIZE_MAX / sizeof(shclw_diag) ? (shclw_diag *)shclw_i_alloc(total * sizeof(shclw_diag)) : NULL;
	shclw_diag *out = NULL;
	if (views) {
		size_t at = 0;
		for (size_t i = 0; i < d->n_layers; i++) {
			for (size_t j = 0; each[i] && j < counts[i]; j++) {
				views[at] = each[i][j];
				views[at++].layer = i + 1;
			}
		}
		out = shclw_i_copy_diags(views, total, n);
		free(views);
	}
	for (size_t i = 0; each && i < d->n_layers; i++) {
		free(each[i]);
	}
	free((void *)each);
	free(counts);
	return out;
}

shclw_diag *shclw_diags(shclw_doc *d, size_t *n) {
	*n = 0;
	return d->layers ? layer_diags(d, n) : own_diags(d, n);
}

static shclw_diag *own_diags(shclw_doc *d, size_t *n) {
	*n = 0;
	if (d->refused) {
		return shclw_i_copy_diags(d->refused, d->n_refused, n);
	}
	if (d->loaded) {
		return shclw_i_copy_diags(d->loaded, d->n_loaded, n);
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

shclw_spot *shclw_unused(shclw_doc *d, size_t *n) {
	*n = 0;
	size_t k = 0;
	shclw_diag *ds = d->refused ? NULL : shclw_diags(d, &k);
	if (!d->refused && !ds) {
		return NULL;
	}
	shclw_spot *out = shclw_i_unused_of(ds, k, n);
	free(ds);
	return out;
}

shclw_merge *shclw_merged(shclw_doc *d, size_t *n) {
	*n = 0;
	if (!d->layers) {
		shclw_i_source one = {d->src, d->src_len, 0};
		return shclw_i_merges_of(&one, 1, n);
	}
	shclw_i_source *srcs = (shclw_i_source *)calloc(d->n_layers, sizeof(shclw_i_source));
	if (!srcs) {
		return NULL;
	}
	size_t k = 0;
	for (size_t i = 0; i < d->n_layers; i++) {
		if (d->layers[i]) {
			srcs[k].text = d->layers[i]->src;
			srcs[k].len = d->layers[i]->src_len;
			srcs[k].layer = i + 1;
			k++;
		}
	}
	shclw_merge *out = shclw_i_merges_of(srcs, k, n);
	free(srcs);
	return out;
}

// The save's text before the info block and the Format line, and whether it
// keeps the file's lines. Caller frees.
static char *lines_of(shclw_doc *d, size_t *len, int *kept) {
	// Without the lines kept, this is the canonical form.
	shcl_str s = shcl_to_text_keep_lines(d->d, kept);
	char *text = shclw_i_dup(s.p ? s.p : "", s.p ? s.n : 0, len);
	shcl_reads_release(d->d);
	return text;
}

shclw_commented_line *shclw_commented(shclw_doc *d, size_t *n) {
	*n = 0;
	size_t len = 0;
	int kept = 0;
	char *text = lines_of(shclw_i_top(d), &len, &kept);
	if (!text) {
		return NULL;
	}
	shclw_commented_line *out = shclw_i_commented_of(text, len, NULL, n);
	free(text);
	return out;
}

// The line of path's commented "## Default" setting in text, or 0. SIZE_MAX
// when memory ran out.
static size_t default_line(const char *text, size_t len, const char *path, size_t plen) {
	size_t n = 0;
	shclw_commented_line *cs = shclw_i_commented_of(text, len, path, &n);
	if (!cs) {
		return SIZE_MAX;
	}
	size_t line = 0;
	for (size_t i = 0; i < n && !line; i++) {
		if (cs[i].path.n == plen && shclw_i_same_lower(cs[i].path.p, path, plen)) {
			line = cs[i].line;
		}
	}
	free(cs);
	return line;
}

// What a fill-in replaced, so a set that fails can put it back.
typedef struct {
	shcl_doc *was;
	bool first;
} filled;

// Swaps path's commented "## Default" line for the setting, so the set that
// follows takes that line and the ones under it don't move
// (2026101513330020). It's only where the save keeps the file's lines and
// path isn't set yet. undo has what settle takes to put the document back.
static shclw_kind fill(shclw_doc *d, const char *path, size_t plen, filled *undo) {
	undo->was = NULL;
	undo->first = false;
	if (d->refused || shcl_exists(d->d, path, plen)) {
		return SHCLW_OK;
	}
	size_t len = 0;
	int kept = 0;
	char *text = lines_of(d, &len, &kept);
	if (!text) {
		return SHCLW_ERR_NO_MEMORY;
	}
	size_t line = kept ? default_line(text, len, path, plen) : 0;
	size_t up_len = 0;
	char *up = line && line != SIZE_MAX ? shclw_i_uncommented(text, len, line, &up_len) : NULL;
	free(text);
	if (line == 0) {
		return SHCLW_OK;
	}
	if (!up) {
		return SHCLW_ERR_NO_MEMORY;
	}
	shcl_doc *nd = shcl_parse_keep_lines(up, up_len, shcl_strictness_of(d->d));
	free(up);
	if (!nd) {
		return SHCLW_ERR_NO_MEMORY;
	}
	if (shcl_error_count(nd) != shcl_error_count(d->d) || !shcl_exists(nd, path, plen)) {
		shcl_free(nd);
		return SHCLW_OK;
	}
	undo->first = !d->loaded;
	if (undo->first) {
		d->loaded = own_diags(d, &d->n_loaded);
		if (!d->loaded) {
			shcl_free(nd);
			return SHCLW_ERR_NO_MEMORY;
		}
	}
	undo->was = d->d;
	d->d = nd;
	return SHCLW_OK;
}

// Keeps a fill-in when the set after it worked, and puts the document back
// when it didn't.
static void settle(shclw_doc *d, filled *undo, bool ok) {
	if (!undo->was) {
		return;
	}
	if (ok) {
		shcl_free(undo->was);
	} else {
		shcl_free(d->d);
		d->d = undo->was;
		if (undo->first) {
			free(d->loaded);
			d->loaded = NULL;
			d->n_loaded = 0;
		}
	}
	undo->was = NULL;
}

size_t shclw_errors(shclw_doc *d) {
	if (d->layers) {
		size_t n = 0;
		for (size_t i = 0; i < d->n_layers; i++) {
			n += d->layers[i] ? own_errors(d->layers[i]) : 0;
		}
		return n;
	}
	return own_errors(d);
}

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

static shclw_kind set_value(shclw_doc *d, const char *path, size_t plen, const shclw_i_value *v, shclw_error *err) {
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

shclw_kind shclw_i_set(shclw_doc *d, const char *path, const shclw_i_value *v, shclw_error *err) {
	size_t plen = strlen(path);
	filled undo;
	shclw_kind k = fill(d, path, plen, &undo);
	if (k != SHCLW_OK) {
		return shclw_i_fail(err, k, path, NULL, 0);
	}
	k = set_value(d, path, plen, v, err);
	settle(d, &undo, k == SHCLW_OK);
	return k;
}

static shclw_kind put_syntax(shclw_doc *d, const char *path, const char *text, size_t len, shclw_error *err);

shclw_kind shclw_put_syntax(shclw_doc *d, const char *path, const char *text, size_t len, shclw_error *err) {
	shclw_kind k = put_syntax(shclw_i_top(d), path, text, len, err);
	shclw_kind m = shclw_i_edited(d, NULL);
	return m == SHCLW_OK ? k : shclw_i_fail(err, m, path, NULL, 0);
}

static shclw_kind put_syntax(shclw_doc *d, const char *path, const char *text, size_t len, shclw_error *err) {
	if (!text) {
		text = "";
		len = 0;
	}
	filled undo;
	shclw_kind k = fill(d, path, strlen(path), &undo);
	if (k != SHCLW_OK) {
		return shclw_i_fail(err, k, path, NULL, 0);
	}
	k = set_err(err, path, shcl_set_literal(d->d, path, strlen(path), text, len));
	settle(d, &undo, k == SHCLW_OK);
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
	shclw_doc *top = shclw_i_top(d);
	shclw_kind k = set_err(err, path, shcl_set_comment(top->d, path, strlen(path), text ? text : "", text ? len : 0));
	shclw_kind m = shclw_i_edited(d, NULL);
	return m == SHCLW_OK ? k : shclw_i_fail(err, m, path, NULL, 0);
}

// A layered document whose merge can't be built again for want of memory
// keeps the one it had.
size_t shclw_remove(shclw_doc *d, const char *path) {
	size_t n = shcl_remove(shclw_i_top(d)->d, path, strlen(path));
	(void)shclw_i_edited(d, NULL);
	return n;
}

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
	shclw_diag g = {line, SHCLW_SEV_ERROR, {c, code_len}, {m, msg_len}, 0};
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

static const char not_as_read[] = "the upgraded file doesn't read this value as shcl 2.0 read it";

// Whether s reads another way with 2.x's escapes.
static bool v2_changes(const char *s, size_t n) {
	for (size_t i = 0; i + 1 < n; i++) {
		if (s[i] == '\\') {
			char e = s[i + 1];
			if (e == 't' || e == 'n' || e == '\\' || e == '"' || e == '\'') {
				return true;
			}
		}
	}
	return false;
}

// What check_reads checks 2.0's reads against, one element at a time.
typedef struct {
	shcl_doc *now;
	old_list *bad;
	shcl_read_str_arr is;
	bool differs;
} carry_check;

static void check_old(void *ctx, const char *path, size_t plen, size_t line, size_t i, size_t n, const char *v,
                      size_t vlen) {
	carry_check *c = (carry_check *)ctx;
	if (i == 0) {
		c->is = shcl_read_string_array(c->now, path, plen);
		c->differs = false;
	}
	if (c->differs || c->is.status != SHCL_GOOD) {
		return;
	}
	// The walk keeps i < n, but clang's CTU pass starts here without it.
	if (c->is.n != n || i >= n || c->is.values[i].n != vlen || (vlen && memcmp(c->is.values[i].p, v, vlen) != 0)) {
		c->differs = true;
		keep_old(c->bad, line, "W002", not_as_read, sizeof not_as_read - 1);
	}
}

// The lines with a value 2.0 read that the upgraded text doesn't have.
typedef struct {
	shcl_doc *now;
	size_t *v;
	size_t n, cap;
	bool oom;
} lost_list;

static void keep_lost(lost_list *l, size_t line) {
	if (l->oom || !line) {
		return;
	}
	if (l->n == l->cap) {
		size_t cap = l->cap ? l->cap * 2 : 8;
		size_t *grown = cap < SIZE_MAX / sizeof(size_t) ? (size_t *)realloc(l->v, cap * sizeof(size_t)) : NULL;
		if (!grown) {
			l->oom = true;
			return;
		}
		l->v = grown;
		l->cap = cap;
	}
	l->v[l->n++] = line;
}

static void check_lost(void *ctx, const char *path, size_t plen, const size_t *lines, size_t n, int reads) {
	lost_list *l = (lost_list *)ctx;
	size_t has = shcl_count(l->now, path, plen);
	// instances past the ones 3.0 still has are gone
	for (size_t i = has < n ? has : n; i < n; i++) {
		keep_lost(l, lines[i]);
	}
	if (reads && has == 1 && shcl_read_string_array(l->now, path, plen).status != SHCL_GOOD &&
	    shcl_read_raw(l->now, path, plen).status != SHCL_GOOD) {
		keep_lost(l, lines[0]);
	}
}

// Go's checkReads: reads every value 2.0 reads in old back against up, the
// upgraded text. shcl's migrate writes the text 2.x read, backslash escapes
// included (2026101910455900), so this checks its answer rather than trusting
// it. What doesn't read the same goes in bad, and the lines it lost in lost,
// sorted.
static shclw_kind check_reads(shclw_i_v2_doc *old, const char *up, size_t up_len, bool names_as_typed, old_list *bad,
                              lost_list *lost) {
	static const char bad_name[] = "shcl 2.0 read this name with its backslash escapes resolved, and 3.0 reads it "
								   "as typed; rename it and upgrade again";
	shcl_doc *now = shcl_parse(up, up_len);
	if (!now) {
		return SHCLW_ERR_NO_MEMORY;
	}
	shcl_field *fs = NULL;
	size_t nf = shcl_fields(now, &fs);
	for (size_t k = 0; k < nf; k++) {
		if (!names_as_typed && v2_changes(fs[k].name.p, fs[k].name.n)) {
			keep_old(bad, shclw_i_v2_line(old, fs[k].path.p, fs[k].path.n), "W002", bad_name, sizeof bad_name - 1);
		}
	}
	carry_check c = {now, bad, {NULL, 0, SHCL_NOT_FOUND, NULL}, false};
	shclw_i_v2_reads(old, check_old, &c);
	lost->now = now;
	shclw_i_v2_paths(old, check_lost, lost);
	size_t kept = 0;
	if (lost->n) {
		qsort(lost->v, lost->n, sizeof(size_t), by_size);
	}
	for (size_t i = 0; i < lost->n; i++) {
		if (i == 0 || lost->v[i] != lost->v[i - 1]) {
			lost->v[kept++] = lost->v[i];
		}
	}
	lost->n = kept;
	shcl_free(now);
	return lost->oom || bad->oom ? SHCLW_ERR_NO_MEMORY : SHCLW_OK;
}

// What mend_reads gathers of 2.0's read of one path, an element at a time.
typedef struct {
	shcl_doc *now, *d;
	const char *up;
	size_t up_len;
	const char **items;
	size_t *lens;
	size_t cap;
	bool oom, failed;
} mend;

static void mend_old(void *ctx, const char *path, size_t plen, size_t line, size_t i, size_t n, const char *v,
                     size_t vlen) {
	(void)line;
	mend *m = (mend *)ctx;
	if (m->oom || m->failed) {
		return;
	}
	if (i == 0 && n > m->cap) {
		const char **items =
			n < SIZE_MAX / sizeof(char *) ? (const char **)realloc((void *)m->items, n * sizeof(char *)) : NULL;
		if (items) {
			m->items = items;
		}
		size_t *lens = items && n < SIZE_MAX / sizeof(size_t) ? (size_t *)realloc(m->lens, n * sizeof(size_t)) : NULL;
		if (!lens) {
			m->oom = true;
			return;
		}
		m->lens = lens;
		m->cap = n;
	}
	if (i >= m->cap) {
		return;
	}
	m->items[i] = v;
	m->lens[i] = vlen;
	if (i + 1 != n || shcl_count(m->now, path, plen) != 1 || shcl_read_raw(m->now, path, plen).status == SHCL_GOOD) {
		return;
	}
	shcl_read_str_arr is = shcl_read_string_array(m->now, path, plen);
	if (is.status != SHCL_GOOD) {
		return;
	}
	bool same = is.n == n;
	for (size_t k = 0; same && k < n; k++) {
		same = is.values[k].n == m->lens[k] && (!m->lens[k] || !memcmp(is.values[k].p, m->items[k], m->lens[k]));
	}
	if (same) {
		return;
	}
	if (!m->d) {
		m->d = shcl_parse_keep_lines(m->up, m->up_len, SHCL_STANDARD);
	}
	if (!m->d) {
		m->oom = true;
		return;
	}
	// 2.0's string read is Good on a list too, so its length says which.
	shcl_set_status st = n == 1 ? shcl_set_string(m->d, path, plen, m->items[0], m->lens[0])
	                            : shcl_set_string_array(m->d, path, plen, m->items, m->lens, n);
	m->failed = st != SHCL_SET_OK;
}

// Go's mendReads: up with each value that reads otherwise than 2.0 read it
// in old set back to 2.0's reading. shcl's migrate splits a 2.x element at a
// comma inside quotes, as in title="a, b", which 1.2 and 2.0 read whole
// (2026102618065640). check_reads still checks the result. malloc'd, or NULL
// when memory ran out.
static char *mend_reads(shclw_i_v2_doc *old, const char *up, size_t up_len, size_t *out_len) {
	shcl_doc *now = shcl_parse(up, up_len);
	if (!now) {
		return NULL;
	}
	mend m = {now, NULL, up, up_len, NULL, NULL, 0, false, false};
	shclw_i_v2_reads(old, mend_old, &m);
	char *out = NULL;
	if (!m.oom) {
		int kept = 0;
		shcl_str t = {NULL, 0};
		if (m.d && !m.failed) {
			t = shcl_to_text_keep_lines(m.d, &kept);
		}
		out = kept && t.p ? shclw_i_dup(t.p, t.n, out_len) : shclw_i_dup(up, up_len, out_len);
	}
	if (m.d) {
		shcl_free(m.d);
	}
	shcl_free(now);
	free((void *)m.items);
	free(m.lens);
	return out;
}

// The result of an upgrade of text to up, with text's line ends, and with the
// info block put where the mark is when banner. *k is set on a failure.
static shclw_upgraded *upgraded_ends(const char *text, size_t len, const char *up, size_t up_len, bool banner,
                                     bool marked, int64_t was, const shcl_upgraded *u, const char *path, shclw_kind *k,
                                     shclw_error *err) {
	size_t ends_n = 0, t_len = 0;
	char *ends = shclw_i_same_ends(text, len, up, up_len, &info_edit, &ends_n);
	char *t = NULL;
	if (!ends) {
		*k = shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
		return NULL;
	}
	if (!banner) {
		shclw_upgraded *r = shclw_i_upgraded(ends, ends_n, was, true, u->lost, NULL, NULL, 0);
		free(ends);
		return r;
	}
	*k = shclw_i_place_block(ends, ends_n, marked, &info_edit, path, &t, &t_len, err);
	free(ends);
	shclw_upgraded *r = NULL;
	if (*k == SHCLW_OK) {
		bool changed = t_len != len || memcmp(t, text, len) != 0;
		r = shclw_i_upgraded(t, t_len, was, changed, u->lost, NULL, NULL, 0);
	} else {
		r = shclw_i_upgraded(text, len, u->format, false, 0, NULL, NULL, 0);
	}
	free(t);
	return r;
}

// u with its lost and rewritten lines, against text, the original. u is
// freed. NULL only when memory ran out.
static shclw_upgraded *with_lines(shclw_upgraded *u, const char *text, size_t len, const lost_list *lost) {
	size_t *rewritten = NULL, n = 0;
	shclw_upgraded *r = NULL;
	if (shclw_i_rewritten_lines(text, len, u->text.p, u->text.n, lost->v, lost->n, &rewritten, &n)) {
		r = shclw_i_upgraded_lines(u, NULL, lost->v, lost->n, rewritten, n);
	}
	free(rewritten);
	free(u);
	return r;
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
	// The new block goes where 2.x's sat, so a mark shcl leaves be stands in.
	// A file with no Format line that has 2.x's block is 2.x (design.md,
	// Decisions).
	bool banner = false;
	char *marked = NULL;
	size_t marked_len = 0;
	if (named == 0) {
		if (!shclw_i_old_block(text, len, &info_edit, &banner, &marked, &marked_len)) {
			return shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
		}
		if (banner && from == 0) {
			from = 2;
		}
	}
	bool from_v2 = from > 0 && from < SHCLW_FORMAT_MAJOR;
	shcl_upgraded u =
		marked ? shcl_upgrade(marked, marked_len, from_v2 ? 1 : 0) : shcl_upgrade(text, len, from_v2 ? 1 : 0);
	if (!u.text) {
		shcl_upgraded_free(&u);
		free(marked);
		return shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
	}
	int64_t was = u.format;
	// shcl says 2 for a file with no Format line, even one it leaves alone.
	if (u.current && !from_v2 && named == 0) {
		was = SHCLW_FORMAT_MAJOR;
	}
	shclw_kind k = SHCLW_OK;
	if (u.current && !(from_v2 && u.format < SHCLW_FORMAT_MAJOR)) {
		*out = shclw_i_upgraded(text, len, was, false, u.lost, NULL, NULL, 0);
	} else {
		// A file taken as 2.x that 2.0 reports errors on. The old program
		// read those lines as it repaired them, or not at all, and an open raw
		// block took the rest of the file. The upgrade would turn that into a
		// clean file that means something else (design.md, Format and upgrade).
		shclw_i_v2_doc *old = shclw_i_v2_parse(text, len);
		old_list l = {NULL, 0, 0, !old};
		if (old) {
			(void)shclw_i_v2_errors(old, keep_old, &l);
		}
		// Nothing in this file says it's 2.x, only its errors on 3.0. With
		// errors on 2.0 too, it's a current file with a typo (design.md,
		// Decisions).
		bool guessed = !from_v2 && named == 0;
		bool checked = false;
		lost_list lost = {NULL, NULL, 0, 0, false};
		char *fixed = NULL;
		const char *up = u.text;
		size_t up_len = u.len;
		if (!l.oom && !l.n && !u.ambiguous && !u.errors_left_n) {
			if (!u.current) {
				size_t mended_len = 0;
				char *mended = mend_reads(old, u.text, u.len, &mended_len);
				fixed = mended ? shclw_i_keep_blanks(marked ? marked : text, marked ? marked_len : len, mended,
				                                     mended_len, &info_edit, &up_len)
				               : NULL;
				free(mended);
				up = fixed;
			}
			if (up) {
				k = check_reads(old, up, up_len, named == 1, &l, &lost);
				checked = true;
			} else {
				k = SHCLW_ERR_NO_MEMORY;
			}
		}
		shclw_i_v2_free(old);
		if (l.oom || k != SHCLW_OK) {
			k = shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
		} else if (l.n && guessed && !checked) {
			*out = shclw_i_upgraded(text, len, SHCLW_FORMAT_MAJOR, false, 0, NULL, NULL, 0);
		} else if (u.ambiguous > 0) {
			*out = shclw_i_upgraded(text, len, was, !u.current, u.lost, NULL, NULL, 0);
			char why[64];
			(void)snprintf(why, sizeof why, "%zu lines read two ways", u.ambiguous);
			k = shclw_i_fail(err, SHCLW_ERR_AMBIGUOUS, path, why, 0);
		} else if (l.n) {
			*out = shclw_i_upgraded(text, len, u.format, false, 0, NULL, l.v, l.n);
			char why[96];
			if (checked) {
				(void)snprintf(why, sizeof why,
				               "%zu lines don't read as shcl 2.0 read them; fix them and upgrade again", lines_in(&l));
			} else {
				(void)snprintf(why, sizeof why, "shcl 2.0 reports errors on %zu lines; fix them and upgrade again",
				               lines_in(&l));
			}
			k = shclw_i_fail(err, SHCLW_ERR_REFUSED, path, why, 0);
		} else if (u.errors_left_n) {
			// shcl gives the input back then, and this would read it the 2.x way.
			*out = shclw_i_upgraded(text, len, u.format, false, 0, NULL, NULL, 0);
			char why[96];
			(void)snprintf(why, sizeof why,
			               "%zu lines would still have an error once converted; fix them and upgrade again",
			               u.errors_left_n);
			k = shclw_i_fail(err, SHCLW_ERR_REFUSED, path, why, 0);
		} else if (banner || !u.current) {
			*out = upgraded_ends(text, len, up, up_len, banner, marked != NULL, was, &u, path, &k, err);
		} else {
			*out = shclw_i_upgraded(u.text, u.len, was, false, u.lost, NULL, NULL, 0);
		}
		if (checked && k == SHCLW_OK && *out) {
			*out = with_lines(*out, text, len, &lost);
		}
		free(lost.v);
		free(fixed);
		free_old(&l);
	}
	shcl_upgraded_free(&u);
	free(marked);
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
	replacing r = {path, text, len, u->from, 0};
	shclw_i_replacer how = {keep_original, write_new, &r};
	char *backup = NULL;
	k = shclw_i_replace_read(path, text, len, u->text.p, u->text.n, &how, &backup, err);
	free(text);
	if (k != SHCLW_OK) {
		k = none(k, u->from, NULL, out, err);
	} else {
		*out = shclw_i_upgraded_lines(u, backup, u->lost, u->n_lost, u->rewritten, u->n_rewritten);
		k = *out ? k : shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
	}
	free(backup);
	free(u);
	return k;
}
