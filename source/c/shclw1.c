// Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
// SPDX-License-Identifier: MIT

// The wrapper over shcl 1.2.
//
// shcl 1.2 has no save, no keep-lines text, and no upgrade. shclw_text gives
// the canonical form, shclw_write saves it atomically with a write of the
// wrapper's own, and shclw_upgrade only refuses files newer than 1.2 reads.
//
// 1.2 keeps what each read, text, and edit uses until the document is freed,
// and has no call that gives it back (design.md, Decisions).

// The save needs POSIX calls, and 1.2's shcl.h asks for none.
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L // NOLINT(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
#endif

#include "shclw_adapter.h"

#if SHCLW_SHCL != 1
#error "shclw1.c is the adapter for shcl 1.2, and this shcl.h is another version"
#endif

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

struct shclw_doc {
	shcl_doc *d;
	// The loaded text wasn't canonical, so a save changes its lines.
	bool rewrites;
	shclw_i_info info;
	bool stamp;
};

static shclw_status status_of(shcl_status s) {
	switch (s) {
	case SHCL_GOOD: return SHCLW_GOOD;
	case SHCL_EMPTY: return SHCLW_EMPTY;
	case SHCL_NOT_FOUND: return SHCLW_NOT_FOUND;
	case SHCL_BAD_TYPE: return SHCLW_BAD_TYPE;
	case SHCL_MULTIPLE: return SHCLW_MULTIPLE;
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

typedef struct {
	char *p;
	size_t n, cap;
	bool oom;
} bytes;

static void add(bytes *b, const char *p, size_t n) {
	if (b->oom || n == 0) {
		return;
	}
	if (n > b->cap - b->n) {
		size_t cap = b->cap ? b->cap : 256;
		while (cap - b->n < n && cap <= SIZE_MAX / 4) {
			cap *= 2;
		}
		char *grown = cap - b->n >= n ? (char *)realloc(b->p, cap) : NULL;
		if (!grown) {
			b->oom = true;
			return;
		}
		b->p = grown;
		b->cap = cap;
	}
	memcpy(b->p + b->n, p, n);
	b->n += n;
}

// Every path's values and the load's error codes, for shclw_i_finish_info to
// tell a comment from a raw body's line.
static char *values(const char *text, size_t len, size_t *n) {
	*n = 0;
	shcl_doc *d = shcl_parse(text, len);
	if (!d) {
		return NULL;
	}
	bytes b = {NULL, 0, 0, false};
	shcl_str *paths = NULL;
	size_t count = shcl_paths(d, &paths);
	for (size_t i = 0; i < count; i++) {
		add(&b, paths[i].p, paths[i].n);
		shcl_str *vals = NULL;
		size_t nv = shcl_instances(d, paths[i].p, paths[i].n, &vals);
		for (size_t k = 0; k < nv; k++) {
			add(&b, "\0", 1);
			add(&b, vals[k].p, vals[k].n);
		}
		add(&b, "\1", 1);
	}
	for (size_t i = 0; i < shcl_diag_count(d); i++) {
		if (shcl_diag_severity(d, i) == SHCL_SEV_ERROR) {
			const char *code = shcl_diag_code(d, i);
			add(&b, code, strlen(code));
			add(&b, "\2", 1);
		}
	}
	shcl_free(d);
	char *out = b.oom ? NULL : b.p ? b.p : (char *)shclw_i_alloc(1);
	if (!out) {
		free(b.p);
		return NULL;
	}
	*n = b.n;
	return out;
}

static const shclw_i_info_edit info_edit = {SHCLW_I_INFO_TEXT("1", "v1.2.0"), SHCLW_FORMAT_MAJOR, values};

// A file's Format line, read as 3.0 reads it, since 1.2 doesn't.
int64_t shclw_format_of(const char *text, size_t len) {
	int64_t f = 0;
	(void)shclw_i_format_in(text, len, &info_edit, &f);
	return f;
}

static shclw_kind too_new(shclw_error *err, const char *path, int64_t format) {
	char why[128];
	(void)snprintf(why, sizeof why, "format %" PRId64 ", and shcl %s reads up to %d", format, SHCLW_SHCL_VERSION,
	               SHCLW_FORMAT_MAJOR);
	return shclw_i_fail(err, SHCLW_ERR_TOO_NEW, path, why, 0);
}

static shclw_doc *wrap(shcl_doc *sd) {
	shclw_doc *d = sd ? (shclw_doc *)malloc(sizeof *d) : NULL;
	if (!d) {
		shcl_free(sd);
		return NULL;
	}
	d->d = sd;
	d->rewrites = false;
	d->info = SHCLW_I_INFO_AS_IS;
	d->stamp = false;
	return d;
}

shclw_doc *shclw_new(void) { return wrap(shcl_new()); }

shclw_doc *shclw_parse(const char *text, size_t len) {
	shclw_doc *d = NULL;
	shclw_parse_with(text, len, SHCLW_STANDARD, &d, NULL);
	return d;
}

shclw_kind shclw_parse_with(const char *text, size_t len, shclw_strictness level, shclw_doc **doc, shclw_error *err) {
	if (!text) {
		text = "";
		len = 0;
	}
	shclw_doc *d = wrap(shcl_parse_with(text, len, level_of(level)));
	*doc = d;
	if (!d) {
		return shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, NULL, NULL, 0);
	}
	shcl_str canonical = shcl_to_canonical(d->d);
	d->rewrites = canonical.n != len || (len && memcmp(canonical.p, text, len) != 0);
	if (shcl_strict_failed(d->d)) {
		char why[64];
		(void)snprintf(why, sizeof why, "the load has %zu errors", shcl_error_count(d->d));
		return shclw_i_fail(err, SHCLW_ERR_STRICT, NULL, why, 0);
	}
	return shclw_i_ok(err);
}

shclw_kind shclw_open(const char *path, shclw_doc **doc, shclw_error *err) {
	return shclw_open_with(path, SHCLW_STANDARD, doc, err);
}

shclw_kind shclw_open_with(const char *path, shclw_strictness level, shclw_doc **doc, shclw_error *err) {
	*doc = NULL;
	char *text = NULL;
	size_t len = 0;
	shclw_kind kind = shclw_read_text(path, &text, &len, err);
	if (kind == SHCLW_OK) {
		int64_t format = 0;
		kind = shclw_i_format_in(text, len, &info_edit, &format);
		if (kind != SHCLW_OK) {
			kind = shclw_i_fail(err, kind, path, NULL, 0);
		} else if (format > SHCLW_FORMAT_MAJOR) {
			kind = too_new(err, path, format);
		}
	}
	if (kind != SHCLW_OK) {
		free(text);
		*doc = shclw_new();
		return *doc ? kind : shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
	}
	kind = shclw_parse_with(text, len, level, doc, NULL);
	free(text);
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
		free(d);
	}
}

shcl_doc *shclw_inner(shclw_doc *d) { return d->d; }

// Nothing to give back with, 1.2 keeps it all until shcl_free.
void shclw_compact(shclw_doc *d) { (void)d; }

// The save's text, with the info block and the Format line as asked, or
// without them when an edit since made them impossible; shclw_write says why.
static char *finish(shclw_doc *d, size_t *len) {
	shcl_str s = shcl_to_canonical(d->d);
	const char *text = s.p ? s.p : "";
	size_t text_len = s.p ? s.n : 0;
	char *out = NULL;
	size_t n = 0;
	shclw_kind k = shclw_i_finish_info(text, text_len, d->info, d->stamp, &info_edit, NULL, &out, &n, NULL);
	if (k != SHCLW_OK && k != SHCLW_ERR_NO_MEMORY) {
		out = shclw_i_dup(text, text_len, &n);
	}
	if (len) {
		*len = out ? n : 0;
	}
	return out;
}

char *shclw_text(shclw_doc *d, size_t *len) { return finish(d, len); }

char *shclw_canonical(shclw_doc *d, size_t *len) { return finish(d, len); }

// 1.2 only saves canonical form, so that's any loaded text that wasn't
// canonical already. A new document has no lines to keep.
bool shclw_rewrites(shclw_doc *d) { return d->rewrites; }

// What the save would write with info and stamp, thrown away: InfoBlock and
// StampFormat refuse what it refuses.
static shclw_kind try_info(shclw_doc *d, shclw_i_info info, bool stamp, shclw_error *err) {
	shcl_str s = shcl_to_canonical(d->d);
	char *out = NULL;
	size_t n = 0;
	shclw_kind k = shclw_i_finish_info(s.p ? s.p : "", s.n, info, stamp, &info_edit, NULL, &out, &n, err);
	free(out);
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

#ifdef _WIN32
static shclw_kind write_atomic(const char *path, const char *data, size_t len, shclw_error *err) {
	if (shclw_i_irregular(path)) {
		return shclw_i_fail(err, SHCLW_ERR_IO, path, "not a regular file", 0);
	}
	const char *slash = strrchr(path, '/');
	const char *back = strrchr(path, '\\');
	slash = back > slash ? back : slash;
	int dir_len = slash ? (int)(slash - path + 1) : 0;
	size_t cap = strlen(path) + 48;
	char *tmp = (char *)malloc(cap);
	if (!tmp) {
		return shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
	}
	int fd = -1;
	for (int attempt = 0; attempt < 8 && fd < 0; attempt++) {
		(void)snprintf(tmp, cap, "%.*s.%s.tmp%d.%d", dir_len, path, path + dir_len, _getpid(), attempt);
		fd = _open(tmp, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
	}
	if (fd < 0) {
		int e = errno;
		free(tmp);
		return shclw_i_fail(err, SHCLW_ERR_IO, path, NULL, e);
	}
	int e = 0;
	for (size_t at = 0; at < len;) {
		unsigned chunk = len - at > 1u << 30 ? 1u << 30 : (unsigned)(len - at);
		int w = _write(fd, data + at, chunk);
		if (w <= 0) {
			e = w < 0 ? errno : EIO;
			break;
		}
		at += (size_t)w;
	}
	if (!e && _commit(fd) != 0) {
		e = errno;
	}
	if (_close(fd) != 0 && !e) {
		e = errno;
	}
	// rename() won't replace a file on Windows.
	bool moved = !e && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
	if (!moved) {
		(void)remove(tmp);
	}
	free(tmp);
	if (moved) {
		return shclw_i_ok(err);
	}
	return shclw_i_fail(err, SHCLW_ERR_IO, path, e ? NULL : "couldn't replace the file", e);
}
#else
// Go's WriteAtomic: a temp file beside path, renamed into place, so a reader
// sees the old file or the new one. An existing file keeps its mode, and also
// a new one is 0644, as in Go.
static shclw_kind write_atomic(const char *path, const char *data, size_t len, shclw_error *err) {
	mode_t mode = 0644;
	struct stat st;
	if (stat(path, &st) == 0) {
		if (!S_ISREG(st.st_mode)) {
			return shclw_i_fail(err, SHCLW_ERR_IO, path, "not a regular file", 0);
		}
		mode = st.st_mode & 0777;
	}
	const char *slash = strrchr(path, '/');
	int dir_len = slash ? (int)(slash - path + 1) : 0;
	size_t cap = strlen(path) + 16;
	char *tmp = (char *)malloc(cap);
	if (!tmp) {
		return shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
	}
	(void)snprintf(tmp, cap, "%.*s.%s.tmpXXXXXX", dir_len, path, path + dir_len);
	int fd = mkstemp(tmp);
	if (fd < 0) {
		int e = errno;
		free(tmp);
		return shclw_i_fail(err, SHCLW_ERR_IO, path, NULL, e);
	}
	int e = 0;
	for (size_t at = 0; at < len;) {
		ssize_t w = write(fd, data + at, len - at);
		if (w < 0 && errno == EINTR) {
			continue;
		}
		if (w <= 0) {
			e = w < 0 ? errno : EIO;
			break;
		}
		at += (size_t)w;
	}
	if (!e && fchmod(fd, mode) != 0) {
		e = errno;
	}
	if (!e && fsync(fd) != 0) {
		e = errno;
	}
	if (close(fd) != 0 && !e) {
		e = errno;
	}
	if (!e && rename(tmp, path) != 0) {
		e = errno;
	}
	if (e) {
		(void)remove(tmp);
	}
	free(tmp);
	return e ? shclw_i_fail(err, SHCLW_ERR_IO, path, NULL, e) : shclw_i_ok(err);
}
#endif

shclw_kind shclw_write(shclw_doc *d, const char *path, shclw_error *err) {
	shcl_str s = shcl_to_canonical(d->d);
	char *text = NULL;
	size_t len = 0;
	shclw_kind k = shclw_i_finish_info(s.p ? s.p : "", s.n, d->info, d->stamp, &info_edit, path, &text, &len, err);
	if (k != SHCLW_OK) {
		return k;
	}
	k = write_atomic(path, text, len, err);
	free(text);
	return k;
}

static shclw_diag diag_of(size_t line, shcl_severity sev, const char *code, shcl_str msg) {
	shclw_diag g = {
		line, sev == SHCL_SEV_ERROR ? SHCLW_SEV_ERROR : SHCLW_SEV_HINT, {code, strlen(code)}, {msg.p, msg.n}};
	return g;
}

shclw_diag *shclw_diags(shclw_doc *d, size_t *n) {
	*n = 0;
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

size_t shclw_errors(shclw_doc *d) { return shcl_error_count(d->d); }

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
	return out;
}

// 1.2 has no reopen hints to drop.
void shclw_drop_declared(shclw_doc *schema, shclw_doc *d) { shcl_suppress_declared_repeats(schema->d, d->d); }

int64_t shclw_int(shclw_doc *d, const char *path, int64_t fallback) {
	return shcl_get_int(d->d, path, strlen(path), fallback);
}

// Through shclw_get_float, so infinity gets the fallback too.
double shclw_float(shclw_doc *d, const char *path, double fallback) {
	shclw_got_float g = shclw_get_float(d, path);
	return g.status == SHCLW_GOOD ? g.value : fallback;
}

bool shclw_bool(shclw_doc *d, const char *path, bool fallback) {
	return shcl_get_bool(d->d, path, strlen(path), fallback ? 1 : 0) != 0;
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

// shcl's C reads leave the line out of their result. shcl_line gives it for
// a path that names one node, which is when Go's and Rust's reads have one.
static size_t line_of(shclw_doc *d, const char *path, size_t plen) { return shcl_line(d->d, path, plen); }

shclw_got_int shclw_get_int(shclw_doc *d, const char *path) {
	size_t plen = strlen(path);
	shcl_read_i64 r = shcl_read_int(d->d, path, plen);
	shclw_got_int g = {r.value, status_of(r.status), line_of(d, path, plen)};
	return g;
}

shclw_got_float shclw_get_float(shclw_doc *d, const char *path) {
	size_t plen = strlen(path);
	shcl_read_f64 r = shcl_read_float(d->d, path, plen);
	shclw_got_float g = {r.value, status_of(r.status), line_of(d, path, plen)};
	shclw_i_finite(&g);
	return g;
}

shclw_got_bool shclw_get_bool(shclw_doc *d, const char *path) {
	size_t plen = strlen(path);
	shcl_read_bool r = shcl_read_bool_(d->d, path, plen);
	shclw_got_bool g = {r.value != 0, status_of(r.status), line_of(d, path, plen)};
	return g;
}

static shclw_got_str got_str(shclw_doc *d, const char *path, size_t plen, shcl_read_str r) {
	shclw_got_str g = {NULL, 0, status_of(r.status), line_of(d, path, plen)};
	g.value = shclw_i_dup(r.value.p, r.value.n, &g.len);
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
	return g;
}

shclw_got_ints shclw_get_ints(shclw_doc *d, const char *path) {
	size_t plen = strlen(path);
	shcl_read_i64_arr r = shcl_read_int_array(d->d, path, plen);
	shclw_got_ints g = {NULL, 0, status_of(r.status), line_of(d, path, plen)};
	g.value = shclw_i_copy_ints(r.values, r.values ? r.n : 0, &g.n);
	return g;
}

shclw_got_floats shclw_get_floats(shclw_doc *d, const char *path) {
	size_t plen = strlen(path);
	shcl_read_f64_arr r = shcl_read_float_array(d->d, path, plen);
	shclw_got_floats g = {NULL, 0, status_of(r.status), line_of(d, path, plen)};
	g.value = shclw_i_copy_floats(r.values, r.values ? r.n : 0, &g.n);
	shclw_i_finite_all(&g);
	return g;
}

bool shclw_has(shclw_doc *d, const char *path) { return shcl_exists(d->d, path, strlen(path)) != 0; }

size_t shclw_count(shclw_doc *d, const char *path) { return shcl_count(d->d, path, strlen(path)); }

size_t shclw_line(shclw_doc *d, const char *path) { return shcl_line(d->d, path, strlen(path)); }

// shcl's names under a path that names one node, repeats included.
static shclw_string *children(shclw_doc *d, const char *path, size_t *n) {
	*n = 0;
	shcl_str *kids = NULL;
	size_t count = shcl_children(d->d, path, strlen(path), &kids);
	shclw_string *names = (shclw_string *)shclw_i_alloc(count * sizeof(shclw_string));
	if (!names) {
		return NULL;
	}
	for (size_t i = 0; i < count; i++) {
		names[i].p = kids[i].p;
		names[i].n = kids[i].n;
	}
	shclw_string *out = shclw_i_copy_strs(names, count, n);
	free(names);
	return out;
}

// With several instances 1.2 lists none, so it's every instance's children,
// as in 3.0.
shclw_string *shclw_names(shclw_doc *d, const char *path, size_t *n) {
	*n = 0;
	if (!path[0] || shclw_count(d, path) <= 1) {
		size_t count = 0;
		shclw_string *all = children(d, path, &count);
		shclw_string *out = all ? shclw_i_dedupe_names(all, count, n) : NULL;
		free(all);
		return out;
	}
	size_t count = 0;
	shclw_node *nodes = shclw_each(d, path, &count);
	if (!nodes) {
		return NULL;
	}
	shclw_string *all = NULL;
	shclw_string **blocks = (shclw_string **)calloc(count ? count : 1, sizeof(shclw_string *));
	size_t *sizes = (size_t *)calloc(count ? count : 1, sizeof(size_t));
	size_t total = 0;
	bool oom = !blocks || !sizes;
	for (size_t i = 0; i < count && !oom; i++) {
		blocks[i] = children(d, nodes[i].path.p, &sizes[i]);
		oom = !blocks[i];
		total += sizes[i];
	}
	all = oom ? NULL : (shclw_string *)shclw_i_alloc(total * sizeof(shclw_string));
	shclw_string *out = NULL;
	if (all) {
		size_t at = 0;
		for (size_t i = 0; i < count; i++) {
			if (sizes[i]) {
				memcpy(all + at, blocks[i], sizes[i] * sizeof(shclw_string));
			}
			at += sizes[i];
		}
		out = shclw_i_dedupe_names(all, total, n);
	}
	for (size_t i = 0; blocks && i < count; i++) {
		free(blocks[i]);
	}
	free((void *)blocks);
	free(sizes);
	free(all);
	free(nodes);
	return out;
}

// 1.2 has no instance paths, so they're built from counts and child names
// (shclw_i_each_paths).
shclw_node *shclw_each(shclw_doc *d, const char *path, size_t *n) {
	*n = 0;
	size_t count = 0;
	char **paths = shclw_i_each_paths(d, children, path, &count);
	if (!paths) {
		return NULL;
	}
	shclw_node *nodes = (shclw_node *)shclw_i_alloc(count * sizeof(shclw_node));
	shclw_node *out = NULL;
	if (nodes) {
		for (size_t i = 0; i < count; i++) {
			if (!paths[i]) {
				free(nodes);
				shclw_i_free_paths(paths, count);
				return NULL;
			}
			size_t plen = strlen(paths[i]);
			shcl_str *vals = NULL;
			size_t nv = shcl_instances(d->d, paths[i], plen, &vals);
			nodes[i].path.p = paths[i];
			nodes[i].path.n = plen;
			nodes[i].value.p = nv ? vals[0].p : "";
			nodes[i].value.n = nv ? vals[0].n : 0;
			nodes[i].line = shcl_line(d->d, paths[i], plen);
		}
		out = shclw_i_copy_nodes(nodes, count, n);
		free(nodes);
	}
	shclw_i_free_paths(paths, count);
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
	return out;
}

static const char *reason_name(shcl_write_reason r) {
	switch (r) {
	case SHCL_W_WRITABLE: return "Writable";
	case SHCL_W_BAD_PATH: return "BadPath";
	case SHCL_W_VALUE_IN_PATH: return "ValueInPath";
	case SHCL_W_WILDCARD: return "Wildcard";
	case SHCL_W_NO_SUCH_INDEX: return "NoSuchIndex";
	case SHCL_W_TOO_DEEP: return "TooDeep";
	}
	return "Writable";
}

// A setter's bare 0, made a kind with shcl's reason.
static shclw_kind set_err(shclw_doc *d, const char *path, int ok, shclw_error *err) {
	if (ok) {
		return shclw_i_ok(err);
	}
	shcl_write_reason r = shcl_write_reason_(d->d, path, strlen(path));
	if (r != SHCL_W_WRITABLE) {
		return shclw_i_fail(err, SHCLW_ERR_BAD_PATH, path, reason_name(r), 0);
	}
	return shclw_i_fail(err, SHCLW_ERR_BAD_VALUE, path, NULL, 0);
}

static int set_strs(shclw_doc *d, const char *path, size_t plen, const shclw_i_value *v, bool *oom) {
	const char **items = (const char **)shclw_i_alloc(v->n * sizeof(char *));
	size_t *lens = (size_t *)shclw_i_alloc(v->n * sizeof(size_t));
	if (!items || !lens || v->n > SIZE_MAX / sizeof(char *)) {
		free((void *)items);
		free(lens);
		*oom = true;
		return 0;
	}
	for (size_t i = 0; i < v->n; i++) {
		items[i] = v->strs[i].p ? v->strs[i].p : "";
		lens[i] = v->strs[i].p ? v->strs[i].n : 0;
	}
	int ok = shcl_set_string_array(d->d, path, plen, items, lens, v->n);
	free((void *)items);
	free(lens);
	return ok;
}

// Put, PutSyntax, and Comment all check first that the path names one
// instance at every step. shcl 1.2 would edit the first one and say ok, where
// 3.0 refuses.
shclw_kind shclw_i_set(shclw_doc *d, const char *path, const shclw_i_value *v, shclw_error *err) {
	shclw_kind k = shclw_i_one_target(d, path, err);
	if (k != SHCLW_OK) {
		return k;
	}
	size_t plen = strlen(path);
	int ok = 0;
	bool oom = false;
	switch (v->kind) {
	case SHCLW_I_INT: ok = shcl_set_int(d->d, path, plen, v->i); break;
	case SHCLW_I_FLOAT: ok = shcl_set_float(d->d, path, plen, v->f); break;
	case SHCLW_I_BOOL: ok = shcl_set_bool(d->d, path, plen, v->b ? 1 : 0); break;
	case SHCLW_I_STR: ok = shcl_set_string(d->d, path, plen, v->str, v->n); break;
	case SHCLW_I_STRS: ok = set_strs(d, path, plen, v, &oom); break;
	case SHCLW_I_INTS: ok = shcl_set_int_array(d->d, path, plen, v->ints, v->n); break;
	case SHCLW_I_FLOATS: ok = shcl_set_float_array(d->d, path, plen, v->floats, v->n); break;
	}
	if (oom) {
		return shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
	}
	return set_err(d, path, ok, err);
}

shclw_kind shclw_put_syntax(shclw_doc *d, const char *path, const char *text, size_t len, shclw_error *err) {
	if (!text) {
		text = "";
		len = 0;
	}
	shclw_kind k = shclw_i_one_target(d, path, err);
	if (k != SHCLW_OK) {
		return k;
	}
	k = set_err(d, path, shcl_set_literal(d->d, path, strlen(path), text, len), err);
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
	shclw_kind k = shclw_i_one_target(d, path, err);
	if (k != SHCLW_OK) {
		return k;
	}
	return set_err(d, path, shcl_set_comment(d->d, path, strlen(path), text ? text : "", text ? len : 0), err);
}

size_t shclw_remove(shclw_doc *d, const char *path) { return shcl_remove(d->d, path, strlen(path)); }

// 1.2 has nothing to convert. It hands the text back, or refuses a file newer
// than 1.2 reads.
static shclw_kind upgrade(const char *text, size_t len, int64_t from, const char *path, shclw_upgraded **out,
                          shclw_error *err) {
	*out = NULL;
	if (!text) {
		text = "";
		len = 0;
	}
	int64_t named = 0;
	if (shclw_i_format_in(text, len, &info_edit, &named) != SHCLW_OK) {
		return shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
	}
	int64_t f = named > from ? named : from;
	if (f > SHCLW_FORMAT_MAJOR) {
		*out = shclw_i_upgraded(text, len, f, false, 0, NULL, NULL, 0);
		return *out ? too_new(err, path, f) : shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
	}
	*out = shclw_i_upgraded(text, len, SHCLW_FORMAT_MAJOR, false, 0, NULL, NULL, 0);
	return *out ? shclw_i_ok(err) : shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
}

shclw_kind shclw_upgrade(const char *text, size_t len, int64_t from, shclw_upgraded **out, shclw_error *err) {
	return upgrade(text, len, from, NULL, out, err);
}

// The result of a file upgrade that failed: only the format.
static shclw_kind none(shclw_kind k, int64_t from, shclw_upgraded **out, shclw_error *err) {
	*out = shclw_i_upgraded("", 0, from, false, 0, NULL, NULL, 0);
	return *out ? k : shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, NULL, NULL, 0);
}

// shclw_upgrade on a file. It never writes, since nothing changes.
shclw_kind shclw_upgrade_file(const char *path, int64_t from, shclw_upgraded **out, shclw_error *err) {
	*out = NULL;
	if (from > SHCLW_FORMAT_MAJOR) {
		return none(too_new(err, path, from), from, out, err);
	}
	// A FIFO would block the read.
	if (shclw_i_irregular(path)) {
		return none(shclw_i_fail(err, SHCLW_ERR_IO, path, "not a regular file", 0), 0, out, err);
	}
	char *text = NULL;
	size_t len = 0;
	shclw_kind k = shclw_read_text(path, &text, &len, err);
	if (k != SHCLW_OK) {
		return none(k, 0, out, err);
	}
	shclw_upgraded *u = NULL;
	k = upgrade(text, len, from, path, &u, err);
	free(text);
	if (!u || k == SHCLW_OK) {
		*out = u;
		return k;
	}
	k = none(k, u->from, out, err);
	free(u);
	return k;
}
