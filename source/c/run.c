// Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
// SPDX-License-Identifier: MIT

// shclw_run: the the ops script runner, a port of Go's shclw.Run. Its output has
// to match Go's byte for byte.

// opendir is POSIX.
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L // NOLINT(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
#endif

#include "shclw_adapter.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#else
#include <dirent.h>
#endif

typedef struct {
	char *p;
	size_t n, cap;
	bool oom;
} buf;

static void put(buf *b, const char *p, size_t n) {
	if (b->oom) {
		return;
	}
	if (n >= b->cap - b->n) {
		size_t cap = b->cap ? b->cap : 256;
		while (n >= cap - b->n) {
			if (cap > SIZE_MAX / 2) {
				b->oom = true;
				return;
			}
			cap *= 2;
		}
		char *grown = (char *)realloc(b->p, cap);
		if (!grown) {
			b->oom = true;
			return;
		}
		b->p = grown;
		b->cap = cap;
	}
	if (n) {
		memcpy(b->p + b->n, p, n);
	}
	b->n += n;
	b->p[b->n] = '\0';
}

static void puts_(buf *b, const char *z) { put(b, z, strlen(z)); }
static void putc_(buf *b, char c) { put(b, &c, 1); }
static void nl(buf *b) { putc_(b, '\n'); }

static void put_i64(buf *b, int64_t v) {
	char s[24];
	(void)snprintf(s, sizeof s, "%" PRId64, v);
	puts_(b, s);
}

static void put_size(buf *b, size_t v) {
	char s[24];
	(void)snprintf(s, sizeof s, "%zu", v);
	puts_(b, s);
}

// esc makes a value safe for one tab-separated output field. Bytes that
// aren't valid UTF-8 print as \xHH.
static void esc(buf *b, const char *s, size_t n) {
	char hex[8];
	for (size_t i = 0; i < n;) {
		uint32_t c = 0;
		size_t size = shclw_i_utf8_next(s + i, n - i, &c);
		if (!size) {
			(void)snprintf(hex, sizeof hex, "\\x%02x", (unsigned)(unsigned char)s[i]);
			puts_(b, hex);
			i++;
			continue;
		}
		switch (c) {
		case '\\': puts_(b, "\\\\"); break;
		case '\t': puts_(b, "\\t"); break;
		case '\n': puts_(b, "\\n"); break;
		case '\r': puts_(b, "\\r"); break;
		default:
			if (c < 0x20 || c == 0x7f) {
				(void)snprintf(hex, sizeof hex, "\\x%02x", (unsigned)c);
				puts_(b, hex);
			} else {
				put(b, s + i, size);
			}
		}
		i += size;
	}
}

// The same in every wrapper language: shortest digits, positional from 1e-5
// to 1e17, else with an exponent.
static void put_float(buf *b, double f) {
	if (isnan(f)) {
		puts_(b, "nan");
		return;
	}
	if (isinf(f)) {
		puts_(b, f > 0 ? "inf" : "-inf");
		return;
	}
	char digits[40];
	int exp = 0;
	shclw_i_shortest(f, false, digits, &exp);
	size_t n = strlen(digits);
	while (n > 1 && digits[n - 1] == '0') {
		digits[--n] = '\0';
	}
	if (signbit(f)) {
		putc_(b, '-');
	}
	if (exp >= 17 || exp < -5) {
		putc_(b, digits[0]);
		if (n > 1) {
			putc_(b, '.');
			put(b, digits + 1, n - 1);
		}
		putc_(b, 'e');
		put_i64(b, exp);
	} else if (exp < 0) {
		puts_(b, "0.");
		for (int i = 0; i < -exp - 1; i++) {
			putc_(b, '0');
		}
		put(b, digits, n);
	} else if (n <= (size_t)exp + 1) {
		put(b, digits, n);
		for (size_t i = n; i < (size_t)exp + 1; i++) {
			putc_(b, '0');
		}
		puts_(b, ".0");
	} else {
		put(b, digits, (size_t)exp + 1);
		putc_(b, '.');
		put(b, digits + exp + 1, n - (size_t)exp - 1);
	}
}

// Go's strconv.ParseInt(s, 10, 64).
static bool parse_int(const char *s, int64_t *out) {
	bool neg = *s == '-';
	if (*s == '-' || *s == '+') {
		s++;
	}
	if (!*s) {
		return false;
	}
	uint64_t v = 0;
	for (; *s; s++) {
		if (*s < '0' || *s > '9') {
			return false;
		}
		unsigned digit = (unsigned)(*s - '0');
		if (v > (UINT64_MAX - digit) / 10) {
			return false;
		}
		v = v * 10 + digit;
	}
	if (neg) {
		if (v > (uint64_t)INT64_MAX + 1) {
			return false;
		}
		*out = v == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)v;
	} else {
		if (v > (uint64_t)INT64_MAX) {
			return false;
		}
		*out = (int64_t)v;
	}
	return true;
}

// Plain decimal and exponent forms, plus nan, inf, and -inf. A number too big
// is infinity, as Rust reads it.
static bool parse_float(const char *s, double *out) {
	if (!strcmp(s, "nan")) {
		*out = NAN;
		return true;
	}
	if (!strcmp(s, "inf") || !strcmp(s, "-inf")) {
		*out = *s == '-' ? -INFINITY : INFINITY;
		return true;
	}
	if (!*s || !((*s >= '0' && *s <= '9') || *s == '.' || *s == '-' || *s == '+')) {
		return false;
	}
	for (const char *p = s; *p; p++) {
		bool letter = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z');
		if (*p == '_' || (letter && *p != 'e' && *p != 'E')) {
			return false;
		}
	}
	char *end = NULL;
	errno = 0;
	*out = strtod(s, &end);
	return end && *end == '\0' && end != s;
}

typedef struct {
	char **v;
	size_t *len;
	size_t n;
} fields;

// Splits a script line on tabs and undoes \t, \n, \r, and \\.
static bool split_fields(const char *line, size_t len, fields *f) {
	static const char escaped[] = "tnr\\";
	static const char unescaped[] = "\t\n\r\\";
	size_t count = 1;
	for (size_t i = 0; i < len; i++) {
		if (line[i] == '\t') {
			count++;
		}
	}
	f->v = (char **)calloc(count, sizeof(char *));
	f->len = (size_t *)calloc(count, sizeof(size_t));
	f->n = 0;
	if (!f->v || !f->len) {
		return false;
	}
	size_t start = 0;
	for (size_t i = 0; i <= len; i++) {
		if (i < len && line[i] != '\t') {
			continue;
		}
		char *field = (char *)malloc(i - start + 1);
		if (!field) {
			return false;
		}
		size_t n = 0;
		for (size_t j = start; j < i;) {
			if (line[j] != '\\' || j + 1 == i) {
				field[n++] = line[j++];
				continue;
			}
			const char *at = (const char *)memchr(escaped, line[j + 1], sizeof escaped - 1);
			if (at) {
				field[n++] = unescaped[at - escaped];
			} else {
				field[n++] = '\\';
				field[n++] = line[j + 1];
			}
			j += 2;
		}
		field[n] = '\0';
		f->len[f->n] = n;
		f->v[f->n++] = field;
		start = i + 1;
	}
	return true;
}

static void free_fields(fields *f) {
	for (size_t i = 0; i < f->n; i++) {
		free(f->v[i]);
	}
	free((void *)f->v);
	free(f->len);
}

// src is the text an open loaded the document from, for check's fresh load,
// and NULL for input.
typedef struct {
	const char *input;
	size_t input_len;
	const char *dir;
	shclw_doc *doc;
	shclw_strictness level;
	buf out;
	char *src;
	size_t src_len;
} runner;

// Fields are NUL-terminated for the C API's paths, which can't hold a NUL.
// Values go by len.
static const char *arg(const fields *f, size_t i) { return i < f->n ? f->v[i] : ""; }
static size_t arg_len(const fields *f, size_t i) { return i < f->n ? f->len[i] : 0; }

static void line_out(runner *r, const char *z) {
	puts_(&r->out, z);
	nl(&r->out);
}

static void result(runner *r, shclw_kind k) {
	if (k == SHCLW_OK) {
		line_out(r, "ok");
		return;
	}
	puts_(&r->out, "error ");
	line_out(r, shclw_kind_name(k));
}

// Multi-line text one line per output line, so a want file diff shows which
// line changed.
static void block(runner *r, const char *text, size_t n) {
	size_t start = 0;
	for (size_t i = 0; i < n; i++) {
		if (text[i] == '\n') {
			puts_(&r->out, "| ");
			esc(&r->out, text + start, i - start);
			nl(&r->out);
			start = i + 1;
		}
	}
	if (start < n) {
		puts_(&r->out, "| ");
		esc(&r->out, text + start, n - start);
		nl(&r->out);
		line_out(r, "(no newline at end)");
	}
}

static void head(buf *b, shclw_status s, size_t line) {
	puts_(b, shclw_status_name(s));
	putc_(b, '\t');
	put_size(b, line);
	putc_(b, '\t');
}

// An array's length goes before its items, so [] and [""] print differently.
static void counted_strs(buf *b, const shclw_string *v, size_t n) {
	put_size(b, n);
	for (size_t i = 0; i < n; i++) {
		putc_(b, '\t');
		esc(b, v[i].p, v[i].n);
	}
}

static void counted_ints(buf *b, const int64_t *v, size_t n) {
	put_size(b, n);
	for (size_t i = 0; i < n; i++) {
		putc_(b, '\t');
		put_i64(b, v[i]);
	}
}

static void counted_floats(buf *b, const double *v, size_t n) {
	put_size(b, n);
	for (size_t i = 0; i < n; i++) {
		putc_(b, '\t');
		put_float(b, v[i]);
	}
}

static void nomem(buf *b) { b->oom = true; }

static void get(runner *r, const char *type, const char *path) {
	buf *b = &r->out;
	shclw_doc *d = r->doc;
	if (!strcmp(type, "int")) {
		shclw_got_int g = shclw_get_int(d, path);
		head(b, g.status, g.line);
		put_i64(b, g.value);
	} else if (!strcmp(type, "float")) {
		shclw_got_float g = shclw_get_float(d, path);
		head(b, g.status, g.line);
		put_float(b, g.value);
	} else if (!strcmp(type, "bool")) {
		shclw_got_bool g = shclw_get_bool(d, path);
		head(b, g.status, g.line);
		puts_(b, g.value ? "true" : "false");
	} else if (!strcmp(type, "str") || !strcmp(type, "raw")) {
		shclw_got_str g = !strcmp(type, "str") ? shclw_get_str(d, path) : shclw_get_raw(d, path);
		if (!g.value) {
			nomem(b);
			return;
		}
		head(b, g.status, g.line);
		esc(b, g.value, g.len);
		free(g.value);
	} else if (!strcmp(type, "strs")) {
		shclw_got_strs g = shclw_get_strs(d, path);
		if (!g.value) {
			nomem(b);
			return;
		}
		head(b, g.status, g.line);
		counted_strs(b, g.value, g.n);
		free(g.value);
	} else if (!strcmp(type, "ints")) {
		shclw_got_ints g = shclw_get_ints(d, path);
		if (!g.value) {
			nomem(b);
			return;
		}
		head(b, g.status, g.line);
		counted_ints(b, g.value, g.n);
		free(g.value);
	} else if (!strcmp(type, "floats")) {
		shclw_got_floats g = shclw_get_floats(d, path);
		if (!g.value) {
			nomem(b);
			return;
		}
		head(b, g.status, g.line);
		counted_floats(b, g.value, g.n);
		free(g.value);
	} else {
		puts_(b, "error badtype");
	}
	nl(b);
}

static void or_(runner *r, const fields *f) {
	buf *b = &r->out;
	shclw_doc *d = r->doc;
	const char *type = arg(f, 1);
	const char *path = arg(f, 2);
	const char *first = arg(f, 3);
	size_t nfall = f->n > 3 ? f->n - 3 : 0;
	if (!strcmp(type, "int")) {
		int64_t v = 0;
		if (!parse_int(first, &v)) {
			line_out(r, "error badvalue");
			return;
		}
		put_i64(b, shclw_int(d, path, v));
	} else if (!strcmp(type, "float")) {
		double v = 0;
		if (!parse_float(first, &v)) {
			line_out(r, "error badvalue");
			return;
		}
		put_float(b, shclw_float(d, path, v));
	} else if (!strcmp(type, "bool")) {
		puts_(b, shclw_bool(d, path, !strcmp(first, "true")) ? "true" : "false");
	} else if (!strcmp(type, "str") || !strcmp(type, "raw")) {
		size_t len = 0;
		char *v = !strcmp(type, "str") ? shclw_str(d, path, first, arg_len(f, 3), &len)
		                               : shclw_raw(d, path, first, arg_len(f, 3), &len);
		if (!v) {
			nomem(b);
			return;
		}
		esc(b, v, len);
		free(v);
	} else if (!strcmp(type, "strs")) {
		shclw_string *fall = (shclw_string *)shclw_i_alloc(nfall * sizeof(shclw_string));
		if (!fall) {
			nomem(b);
			return;
		}
		for (size_t i = 0; i < nfall; i++) {
			fall[i].p = f->v[i + 3];
			fall[i].n = f->len[i + 3];
		}
		size_t n = 0;
		shclw_string *v = shclw_strs(d, path, fall, nfall, &n);
		free(fall);
		if (!v) {
			nomem(b);
			return;
		}
		counted_strs(b, v, n);
		free(v);
	} else if (!strcmp(type, "ints") || !strcmp(type, "floats")) {
		bool ints = !strcmp(type, "ints");
		int64_t *ifall = (int64_t *)shclw_i_alloc(nfall * sizeof(int64_t));
		double *ffall = (double *)shclw_i_alloc(nfall * sizeof(double));
		bool ok = ifall && ffall;
		for (size_t i = 0; ok && i < nfall; i++) {
			ok = ints ? parse_int(f->v[i + 3], &ifall[i]) : parse_float(f->v[i + 3], &ffall[i]);
		}
		if (!ok) {
			if (ifall && ffall) {
				line_out(r, "error badvalue");
			} else {
				nomem(b);
			}
			free(ifall);
			free(ffall);
			return;
		}
		size_t n = 0;
		if (ints) {
			int64_t *v = shclw_ints(d, path, ifall, nfall, &n);
			if (!v) {
				nomem(b);
				free(ifall);
				free(ffall);
				return;
			}
			counted_ints(b, v, n);
			free(v);
		} else {
			double *v = shclw_floats(d, path, ffall, nfall, &n);
			if (!v) {
				nomem(b);
				free(ifall);
				free(ffall);
				return;
			}
			counted_floats(b, v, n);
			free(v);
		}
		free(ifall);
		free(ffall);
	} else {
		puts_(b, "error badtype");
	}
	nl(b);
}

static shclw_strictness strictness_of(const char *level) {
	if (!strcmp(level, "loose")) {
		return SHCLW_LOOSE;
	}
	if (!strcmp(level, "strict")) {
		return SHCLW_STRICT;
	}
	return SHCLW_STANDARD;
}

static void parse(runner *r, const fields *f) {
	shclw_strictness s = strictness_of(arg(f, 1));
	shclw_doc *doc = NULL;
	shclw_kind k = shclw_parse_with(r->input, r->input_len, s, &doc, NULL);
	if (!doc) {
		nomem(&r->out);
		return;
	}
	shclw_free(r->doc);
	r->doc = doc;
	r->level = s;
	free(r->src);
	r->src = NULL;
	result(r, k);
}

static void text(runner *r, bool canonical) {
	size_t n = 0;
	char *t = canonical ? shclw_canonical(r->doc, &n) : shclw_text(r->doc, &n);
	if (!t) {
		nomem(&r->out);
		return;
	}
	block(r, t, n);
	free(t);
}

// A line, with its layer in front on a layered document.
static void put_spot(buf *b, size_t line, size_t layer) {
	if (layer) {
		put_size(b, layer);
		putc_(b, ':');
	}
	put_size(b, line);
}

// Prints and frees ds.
static void print_diags(runner *r, shclw_diag *ds, size_t n) {
	if (!ds) {
		nomem(&r->out);
		return;
	}
	for (size_t i = 0; i < n; i++) {
		put_spot(&r->out, ds[i].line, ds[i].layer);
		putc_(&r->out, '\t');
		puts_(&r->out, shclw_severity_name(ds[i].severity));
		putc_(&r->out, '\t');
		put(&r->out, ds[i].code.p, ds[i].code.n);
		putc_(&r->out, '\t');
		esc(&r->out, ds[i].message.p, ds[i].message.n);
		nl(&r->out);
	}
	free(ds);
}

static void diags(runner *r) {
	size_t n = 0;
	shclw_diag *ds = shclw_diags(r->doc, &n);
	print_diags(r, ds, n);
}

// Keeps a script's file names inside dir. Go's filepath.Base check.
static char *file(runner *r, const char *name) {
	bool nested = strchr(name, '/') && strcmp(name, "/") != 0;
	if (!r->dir || !*name || nested || !strcmp(name, "..")) {
		line_out(r, "error unsupported");
		return NULL;
	}
	size_t n = strlen(r->dir) + strlen(name) + 2;
	char *path = (char *)malloc(n);
	if (!path) {
		nomem(&r->out);
		return NULL;
	}
	(void)snprintf(path, n, "%s/%s", r->dir, name);
	return path;
}

static void write_(runner *r, const fields *f) {
	char *path = file(r, arg(f, 1));
	if (!path) {
		return;
	}
	shclw_write_opts o;
	memset(&o, 0, sizeof o);
	bool bad = false;
	for (size_t i = 2; i < f->n && !bad; i++) {
		const char *flag = f->v[i];
		if (!strcmp(flag, "force")) {
			o.force = true;
		} else if (!strcmp(flag, "create")) {
			o.create = true;
		} else if (!strncmp(flag, "mode=", 5) && flag[5] >= '0' && flag[5] <= '7') {
			char *end = NULL;
			unsigned long m = strtoul(flag + 5, &end, 8);
			bad = *end != '\0' || m > 07777777;
			o.mode = (unsigned)m;
		} else {
			bad = true;
		}
	}
	if (bad) {
		free(path);
		line_out(r, "error badvalue");
		return;
	}
	shclw_kind k = shclw_write_with(r->doc, path, &o, NULL);
	char *saved = NULL;
	size_t n = 0;
	if (k == SHCLW_OK) {
		k = shclw_read_text(path, &saved, &n, NULL);
	}
	free(path);
	if (k != SHCLW_OK) {
		result(r, k);
		return;
	}
	line_out(r, "ok");
	block(r, saved, n);
	free(saved);
}

// A file's permission bits.
static void mode(runner *r, const fields *f) {
	char *path = file(r, arg(f, 1));
	if (!path) {
		return;
	}
	struct stat st;
	int rc = stat(path, &st);
	free(path);
	if (rc != 0) {
		line_out(r, "error io");
		return;
	}
	char bits[8];
	(void)snprintf(bits, sizeof bits, "%04o", (unsigned)st.st_mode & 0777u);
	line_out(r, bits);
}

// Writes text to a file as another program would, behind the document's back.
static void scribble(runner *r, const fields *f) {
	char *path = file(r, arg(f, 1));
	if (!path) {
		return;
	}
	const char *text = arg(f, 2);
	size_t n = strlen(text);
	FILE *out = fopen(path, "wb");
	bool ok = out && fwrite(text, 1, n, out) == n;
	ok = (out && fclose(out) == 0) && ok;
	free(path);
	line_out(r, ok ? "ok" : "error io");
}

static void print_doc_diags(runner *r, shclw_doc *d) {
	size_t n = 0;
	shclw_diag *ds = shclw_diags(d, &n);
	print_diags(r, ds, n);
}

// The text an opened document was loaded from: the file, upgraded when the
// open upgraded it.
static char *source_of(const char *path, shclw_doc *d, size_t *len) {
	char *text = NULL;
	*len = 0;
	if (shclw_read_text(path, &text, len, NULL) != SHCLW_OK || !shclw_upgraded_from(d)) {
		return text;
	}
	shclw_upgraded *u = NULL;
	(void)shclw_upgrade(text, *len, 0, &u, NULL);
	free(text);
	text = u ? shclw_i_dup(u->text.p, u->text.n, len) : NULL;
	free(u);
	return text;
}

// An error's diagnostics go after it, as Go and Rust print the error's, and
// the document is then empty.
static void open_(runner *r, const fields *f) {
	char *path = file(r, arg(f, 1));
	if (!path) {
		return;
	}
	shclw_strictness s = strictness_of(arg(f, 2));
	shclw_doc *doc = NULL;
	shclw_kind k = shclw_open_with(path, s, &doc, NULL);
	if (!doc) {
		free(path);
		nomem(&r->out);
		return;
	}
	free(r->src);
	r->src = NULL;
	if (k != SHCLW_OK) {
		result(r, k);
		print_doc_diags(r, doc);
		shclw_free(doc);
		doc = shclw_new();
	} else {
		line_out(r, "ok");
		r->src = source_of(path, doc, &r->src_len);
	}
	free(path);
	if (!doc) {
		nomem(&r->out);
		return;
	}
	shclw_free(r->doc);
	r->doc = doc;
	r->level = s;
}

// File names, and text for a text layer after a "=".
static void layers_(runner *r, const fields *f) {
	size_t n = f->n > 1 ? f->n - 1 : 0;
	shclw_layer *ls = (shclw_layer *)calloc(n ? n : 1, sizeof *ls);
	char **paths = (char **)calloc(n ? n : 1, sizeof *paths);
	bool ok = ls && paths;
	if (!ok) {
		nomem(&r->out);
	}
	for (size_t i = 0; ok && i < n; i++) {
		if (arg(f, i + 1)[0] == '=') {
			ls[i].text = arg(f, i + 1) + 1;
			ls[i].len = arg_len(f, i + 1) - 1;
			continue;
		}
		paths[i] = file(r, arg(f, i + 1));
		ok = paths[i] != NULL;
		ls[i].file = paths[i];
	}
	shclw_doc *doc = NULL;
	if (ok) {
		shclw_kind k = shclw_open_layers(ls, n, &doc, NULL);
		if (doc) {
			result(r, k);
		}
		if (doc && k != SHCLW_OK) {
			// Go and Rust have the diagnostics on the error, not the document.
			print_doc_diags(r, doc);
			shclw_free(doc);
			doc = shclw_new();
		}
		if (doc) {
			free(r->src);
			r->src = NULL;
			shclw_free(r->doc);
			r->doc = doc;
			r->level = SHCLW_STANDARD;
		} else {
			nomem(&r->out);
		}
	}
	for (size_t i = 0; paths && i < n; i++) {
		free(paths[i]);
	}
	free((void *)paths);
	free(ls);
}

// name with STAMP in place of the time in a backup's name.
static void put_masked(buf *b, const char *name) {
	const char *at = strstr(name, "_backup_");
	const char *end = at ? strstr(at + 8, "_format-v") : NULL;
	if (!end) {
		esc(b, name, strlen(name));
		return;
	}
	esc(b, name, (size_t)(at + 8 - name));
	puts_(b, "STAMP");
	esc(b, end, strlen(end));
}

static void upgraded(runner *r) {
	size_t n = 0;
	char *name = shclw_backup_name(r->doc, &n);
	if (!name) {
		nomem(&r->out);
		return;
	}
	puts_(&r->out, "from ");
	put_i64(&r->out, shclw_upgraded_from(r->doc));
	puts_(&r->out, " backup ");
	if (!n) {
		line_out(r, "none");
		free(name);
		return;
	}
	const char *base = name;
	for (const char *c = name; *c; c++) {
		base = *c == '/' || *c == '\\' ? c + 1 : base;
	}
	put_masked(&r->out, base);
	nl(&r->out);
	char *text = NULL;
	size_t len = 0;
	shclw_kind k = shclw_read_text(name, &text, &len, NULL);
	free(name);
	if (k != SHCLW_OK) {
		result(r, k);
		return;
	}
	block(r, text, len);
	free(text);
}

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

// The names in dir, sorted. NULL when it can't be read.
static char **list_dir(const char *dir, size_t *n, bool *oom) {
	*n = 0;
	*oom = false;
	char **names = NULL;
	size_t cap = 0;
#ifdef _WIN32
	size_t plen = strlen(dir) + 3;
	char *pattern = (char *)malloc(plen);
	if (!pattern) {
		*oom = true;
		return NULL;
	}
	(void)snprintf(pattern, plen, "%s/*", dir);
	struct _finddata_t fd;
	intptr_t h = _findfirst(pattern, &fd);
	free(pattern);
	if (h == -1) {
		return NULL;
	}
	int more = 0;
	for (; more == 0 && !*oom; more = _findnext(h, &fd)) {
		const char *got = fd.name;
#else
	DIR *dh = opendir(dir);
	if (!dh) {
		return NULL;
	}
	for (struct dirent *e = readdir(dh); e && !*oom; e = readdir(dh)) {
		const char *got = e->d_name;
#endif
		if (!strcmp(got, ".") || !strcmp(got, "..")) {
			continue;
		}
		if (*n == cap) {
			size_t want = cap ? cap * 2 : 16;
			char **grown = (char **)realloc((void *)names, want * sizeof *names);
			if (!grown) {
				*oom = true;
				break;
			}
			names = grown;
			cap = want;
		}
		names[*n] = shclw_i_dup(got, strlen(got), NULL);
		*oom = !names[*n];
		*n += !*oom;
	}
#ifdef _WIN32
	_findclose(h);
#else
	closedir(dh);
#endif
	if (!names) {
		names = (char **)malloc(sizeof *names);
		*oom = *oom || !names;
	}
	if (*n) {
		qsort((void *)names, *n, sizeof *names, by_name);
	}
	return names;
}

static void files(runner *r) {
	if (!r->dir) {
		line_out(r, "error unsupported");
		return;
	}
	size_t n = 0;
	bool oom = false;
	char **names = list_dir(r->dir, &n, &oom);
	if (!names) {
		if (oom) {
			nomem(&r->out);
		} else {
			result(r, SHCLW_ERR_IO);
		}
		return;
	}
	for (size_t i = 0; i < n; i++) {
		if (!oom) {
			put_masked(&r->out, names[i]);
			nl(&r->out);
		}
		free(names[i]);
	}
	free((void *)names);
	if (oom) {
		nomem(&r->out);
	}
}

// A schema check. Mode "all" adds the load's own diagnostics first, and "drop"
// does that and then drops what the schema declares.
static void check(runner *r, const fields *f) {
	char *path = file(r, arg(f, 1));
	if (!path) {
		return;
	}
	char *text = NULL;
	size_t len = 0;
	shclw_kind k = shclw_read_text(path, &text, &len, NULL);
	free(path);
	if (k != SHCLW_OK) {
		result(r, k);
		return;
	}
	shclw_doc *schema = shclw_parse(text, len);
	free(text);
	if (!schema) {
		nomem(&r->out);
		return;
	}
	const char *mode = arg(f, 2);
	size_t n = 0;
	shclw_diag *ds = NULL;
	if (!strcmp(mode, "drop")) {
		// C's drop works on a document's own list, and the script's document
		// has to keep its hints, so they come off a fresh load of the same
		// text. Only the load's hints can go; the check gives none.
		shclw_doc *fresh = NULL;
		(void)shclw_parse_with(r->src ? r->src : r->input, r->src ? r->src_len : r->input_len, r->level, &fresh, NULL);
		if (fresh) {
			shclw_drop_declared(schema, fresh);
			ds = shclw_diags(fresh, &n);
			print_diags(r, ds, n);
			shclw_free(fresh);
		} else {
			nomem(&r->out);
		}
	} else if (!strcmp(mode, "all")) {
		ds = shclw_diags(r->doc, &n);
		print_diags(r, ds, n);
	}
	ds = shclw_check(r->doc, schema, &n);
	print_diags(r, ds, n);
	shclw_free(schema);
}

static void upgrade(runner *r, const char *from) {
	int64_t n = 0;
	if (*from && !parse_int(from, &n)) {
		line_out(r, "error badvalue");
		return;
	}
	shclw_upgraded *u = NULL;
	shclw_kind k = shclw_upgrade(r->input, r->input_len, n, &u, NULL);
	if (!u) {
		nomem(&r->out);
		return;
	}
	if (k != SHCLW_OK) {
		result(r, k);
	} else {
		puts_(&r->out, "from ");
		put_i64(&r->out, u->from);
		puts_(&r->out, u->changed ? " changed true unsure " : " changed false unsure ");
		put_size(&r->out, u->unsure);
		nl(&r->out);
		block(r, u->text.p, u->text.n);
	}
	free(u);
}

static void counted_sizes(buf *b, const size_t *v, size_t n) {
	put_size(b, n);
	for (size_t i = 0; i < n; i++) {
		putc_(b, '\t');
		put_size(b, v[i]);
	}
}

static void lines(runner *r, const char *path) {
	size_t n = 0;
	size_t *v = shclw_lines(r->doc, path, &n);
	if (!v) {
		nomem(&r->out);
		return;
	}
	counted_sizes(&r->out, v, n);
	nl(&r->out);
	free(v);
}

static void pick_get(runner *r, const fields *f, bool last) {
	char *path = last ? shclw_last(r->doc, arg(f, 2), NULL) : shclw_first(r->doc, arg(f, 2), NULL);
	if (!path) {
		nomem(&r->out);
		return;
	}
	get(r, arg(f, 1), path);
	free(path);
}

static void names(runner *r, const char *path) {
	size_t n = 0;
	shclw_string *v = shclw_names(r->doc, path, &n);
	if (!v) {
		nomem(&r->out);
		return;
	}
	for (size_t i = 0; i < n; i++) {
		if (i) {
			putc_(&r->out, '\t');
		}
		esc(&r->out, v[i].p, v[i].n);
	}
	nl(&r->out);
	free(v);
}

static void each(runner *r, const fields *f, bool with_get) {
	size_t n = 0;
	shclw_node *nodes = shclw_each(r->doc, arg(f, with_get ? 2 : 1), &n);
	if (!nodes) {
		nomem(&r->out);
		return;
	}
	for (size_t i = 0; i < n && !r->out.oom; i++) {
		put_size(&r->out, i);
		putc_(&r->out, '\t');
		if (!with_get) {
			put_size(&r->out, nodes[i].line);
			putc_(&r->out, '\t');
			esc(&r->out, nodes[i].value.p, nodes[i].value.n);
			nl(&r->out);
			continue;
		}
		char *child = shclw_child(r->doc, nodes[i].path.p, arg(f, 3), arg_len(f, 3), NULL);
		if (!child) {
			nomem(&r->out);
			break;
		}
		get(r, arg(f, 1), child);
		free(child);
	}
	free(nodes);
}

// Go's parseValue, then the put. A value that doesn't parse is badvalue.
static void put_(runner *r, const fields *f) {
	const char *type = arg(f, 1);
	const char *path = arg(f, 2);
	const char *first = arg(f, 3);
	size_t nv = f->n > 3 ? f->n - 3 : 0;
	shclw_kind k = SHCLW_ERR_BAD_VALUE;
	if (!strcmp(type, "int")) {
		int64_t v = 0;
		if (parse_int(first, &v)) {
			k = shclw_put_int(r->doc, path, v, NULL);
		}
	} else if (!strcmp(type, "float")) {
		double v = 0;
		if (parse_float(first, &v)) {
			k = shclw_put_float(r->doc, path, v, NULL);
		}
	} else if (!strcmp(type, "bool")) {
		if (!strcmp(first, "true") || !strcmp(first, "false")) {
			k = shclw_put_bool(r->doc, path, !strcmp(first, "true"), NULL);
		}
	} else if (!strcmp(type, "str")) {
		k = shclw_put_str(r->doc, path, first, arg_len(f, 3), NULL);
	} else if (!strcmp(type, "strs")) {
		shclw_string *v = (shclw_string *)shclw_i_alloc(nv * sizeof(shclw_string));
		if (!v) {
			nomem(&r->out);
			return;
		}
		for (size_t i = 0; i < nv; i++) {
			v[i].p = f->v[i + 3];
			v[i].n = f->len[i + 3];
		}
		k = shclw_put_strs(r->doc, path, v, nv, NULL);
		free(v);
	} else if (!strcmp(type, "ints") || !strcmp(type, "floats")) {
		bool ints = !strcmp(type, "ints");
		int64_t *iv = (int64_t *)shclw_i_alloc(nv * sizeof(int64_t));
		double *fv = (double *)shclw_i_alloc(nv * sizeof(double));
		if (!iv || !fv) {
			free(iv);
			free(fv);
			nomem(&r->out);
			return;
		}
		bool ok = true;
		for (size_t i = 0; ok && i < nv; i++) {
			ok = ints ? parse_int(f->v[i + 3], &iv[i]) : parse_float(f->v[i + 3], &fv[i]);
		}
		if (ok) {
			k = ints ? shclw_put_ints(r->doc, path, iv, nv, NULL) : shclw_put_floats(r->doc, path, fv, nv, NULL);
		}
		free(iv);
		free(fv);
	}
	result(r, k);
}

static void op_has(runner *r, const fields *f) { line_out(r, shclw_has(r->doc, arg(f, 1)) ? "true" : "false"); }
static void op_count(runner *r, const fields *f) {
	put_size(&r->out, shclw_count(r->doc, arg(f, 1)));
	nl(&r->out);
}
static void op_line(runner *r, const fields *f) {
	put_size(&r->out, shclw_line(r->doc, arg(f, 1)));
	nl(&r->out);
}
static void op_lines(runner *r, const fields *f) { lines(r, arg(f, 1)); }
static void op_first(runner *r, const fields *f) { pick_get(r, f, false); }
static void op_last(runner *r, const fields *f) { pick_get(r, f, true); }
static void op_names(runner *r, const fields *f) { names(r, arg(f, 1)); }
static void op_each(runner *r, const fields *f) { each(r, f, false); }
static void op_eachget(runner *r, const fields *f) { each(r, f, true); }
static void op_put(runner *r, const fields *f) { put_(r, f); }
static void op_syntax(runner *r, const fields *f) {
	result(r, shclw_put_syntax(r->doc, arg(f, 1), arg(f, 2), arg_len(f, 2), NULL));
}
static void op_comment(runner *r, const fields *f) {
	result(r, shclw_comment(r->doc, arg(f, 1), arg(f, 2), arg_len(f, 2), NULL));
}
static void op_remove(runner *r, const fields *f) {
	put_size(&r->out, shclw_remove(r->doc, arg(f, 1)));
	nl(&r->out);
}
static void op_parse(runner *r, const fields *f) { parse(r, f); }
static void op_text(runner *r, const fields *f) {
	(void)f;
	text(r, false);
}
static void op_canonical(runner *r, const fields *f) {
	(void)f;
	text(r, true);
}
static void op_diags(runner *r, const fields *f) {
	(void)f;
	diags(r);
}
static void op_errors(runner *r, const fields *f) {
	(void)f;
	put_size(&r->out, shclw_errors(r->doc));
	nl(&r->out);
}
static void op_unused(runner *r, const fields *f) {
	(void)f;
	size_t n = 0;
	shclw_spot *spots = shclw_unused(r->doc, &n);
	if (!spots) {
		nomem(&r->out);
		return;
	}
	put_size(&r->out, n);
	for (size_t i = 0; i < n; i++) {
		putc_(&r->out, '\t');
		put_spot(&r->out, spots[i].line, spots[i].layer);
	}
	nl(&r->out);
	free(spots);
}
static void op_merged(runner *r, const fields *f) {
	(void)f;
	size_t n = 0;
	shclw_merge *ms = shclw_merged(r->doc, &n);
	if (!ms) {
		nomem(&r->out);
		return;
	}
	for (size_t i = 0; i < n; i++) {
		esc(&r->out, ms[i].path.p, ms[i].path.n);
		for (size_t j = 0; j < ms[i].n_lines; j++) {
			putc_(&r->out, '\t');
			put_spot(&r->out, ms[i].lines[j], ms[i].layer);
		}
		nl(&r->out);
	}
	free(ms);
}
static void op_get(runner *r, const fields *f) { get(r, arg(f, 1), arg(f, 2)); }
static void op_or(runner *r, const fields *f) { or_(r, f); }
static void op_write(runner *r, const fields *f) { write_(r, f); }
static void op_readonly(runner *r, const fields *f) {
	(void)f;
	line_out(r, shclw_read_only(r->doc) ? "true" : "false");
}
static void op_scribble(runner *r, const fields *f) { scribble(r, f); }
static void op_mode(runner *r, const fields *f) { mode(r, f); }
static void op_rewrites(runner *r, const fields *f) {
	(void)f;
	line_out(r, shclw_rewrites(r->doc) ? "true" : "false");
}
static void op_info(runner *r, const fields *f) {
	const char *on = arg(f, 1);
	if (strcmp(on, "on") != 0 && strcmp(on, "off") != 0) {
		line_out(r, "error badvalue");
		return;
	}
	result(r, shclw_info_block(r->doc, !strcmp(on, "on"), NULL));
}
static void op_stamp(runner *r, const fields *f) {
	(void)f;
	result(r, shclw_stamp_format(r->doc, NULL));
}
static void op_check(runner *r, const fields *f) { check(r, f); }
static void op_format(runner *r, const fields *f) {
	(void)f;
	put_i64(&r->out, shclw_format_of(r->input, r->input_len));
	nl(&r->out);
}
static void op_upgrade(runner *r, const fields *f) { upgrade(r, arg(f, 1)); }
static void op_open(runner *r, const fields *f) { open_(r, f); }
static void op_layers(runner *r, const fields *f) { layers_(r, f); }
static void op_upgraded(runner *r, const fields *f) {
	(void)f;
	upgraded(r);
}
static void op_files(runner *r, const fields *f) {
	(void)f;
	files(r);
}

// Every op in design.md's "Ops scripts".
// clang-format off
static const struct {
	const char *name;
	void (*run)(runner *, const fields *);
} ops[] = {
	{"parse",     op_parse},
	{"text",      op_text},
	{"canonical", op_canonical},
	{"rewrites",  op_rewrites},
	{"diags",     op_diags},
	{"errors",    op_errors},
	{"unused",    op_unused},
	{"merged",    op_merged},
	{"get",       op_get},
	{"or",        op_or},
	{"has",       op_has},
	{"count",     op_count},
	{"line",      op_line},
	{"lines",     op_lines},
	{"first",     op_first},
	{"last",      op_last},
	{"names",     op_names},
	{"each",      op_each},
	{"eachget",   op_eachget},
	{"put",       op_put},
	{"syntax",    op_syntax},
	{"comment",   op_comment},
	{"remove",    op_remove},
	{"info",      op_info},
	{"stamp",     op_stamp},
	{"check",     op_check},
	{"format",    op_format},
	{"upgrade",   op_upgrade},
	{"write",     op_write},
	{"open",      op_open},
	{"layers",    op_layers},
	{"upgraded",  op_upgraded},
	{"files",     op_files},
	{"readonly",  op_readonly},
	{"scribble",  op_scribble},
	{"mode",      op_mode},
};
// clang-format on

static void op(runner *r, const fields *f) {
	const char *name = arg(f, 0);
	for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++) {
		if (!strcmp(ops[i].name, name)) {
			ops[i].run(r, f);
			return;
		}
	}
	line_out(r, "error unknownop");
}

// Go's strings.TrimSpace(line) == "": every byte is Unicode white space.
static bool blank(const char *s, size_t n) {
	for (size_t i = 0; i < n;) {
		uint32_t c = 0;
		size_t size = shclw_i_utf8_next(s + i, n - i, &c);
		bool space = (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0x85 || c == 0xA0 || c == 0x1680 ||
		             (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F ||
		             c == 0x3000;
		if (!size || !space) {
			return false;
		}
		i += size;
	}
	return true;
}

char *shclw_run(const char *input, size_t len, const char *script, size_t script_len, const char *dir,
                size_t *out_len) {
	*out_len = 0;
	if (!script) {
		script = "";
		script_len = 0;
	}
	runner r = {input, len, dir, shclw_parse(input, len), SHCLW_STANDARD, {NULL, 0, 0, false}, NULL, 0};
	if (!r.doc) {
		return NULL;
	}
	puts_(&r.out, "");
	for (size_t start = 0; start <= script_len && !r.out.oom;) {
		size_t end = start;
		while (end < script_len && script[end] != '\n') {
			end++;
		}
		size_t n = end - start;
		while (n > 0 && script[start + n - 1] == '\r') {
			n--;
		}
		const char *line = script + start;
		start = end + 1;
		if (blank(line, n) || line[0] == '#') {
			continue;
		}
		puts_(&r.out, "> ");
		put(&r.out, line, n);
		nl(&r.out);
		fields f = {NULL, NULL, 0};
		if (split_fields(line, n, &f)) {
			op(&r, &f);
		} else {
			nomem(&r.out);
		}
		free_fields(&f);
	}
	shclw_free(r.doc);
	free(r.src);
	if (r.out.oom) {
		free(r.out.p);
		return NULL;
	}
	*out_len = r.out.n;
	return r.out.p;
}
