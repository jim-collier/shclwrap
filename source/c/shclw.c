// Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
// SPDX-License-Identifier: MIT

// What every version adapter shares.

// lstat is POSIX, which 3.0's shcl.h also asks for.
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L // NOLINT(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
#endif

#include "shclw_adapter.h"

#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

const char *shclw_status_name(shclw_status s) {
	switch (s) {
	case SHCLW_GOOD: return "Good";
	case SHCLW_EMPTY: return "Empty";
	case SHCLW_NOT_FOUND: return "NotFound";
	case SHCLW_BAD_TYPE: return "BadType";
	case SHCLW_MULTIPLE: return "Multiple";
	case SHCLW_BAD_PATH: return "BadPath";
	}
	return "Status(?)";
}

const char *shclw_kind_name(shclw_kind k) {
	switch (k) {
	case SHCLW_OK: return "ok";
	case SHCLW_ERR_NOT_FOUND: return "notfound";
	case SHCLW_ERR_IO: return "io";
	case SHCLW_ERR_STRICT: return "strict";
	case SHCLW_ERR_REFUSED: return "refused";
	case SHCLW_ERR_BAD_PATH: return "badpath";
	case SHCLW_ERR_BAD_VALUE: return "badvalue";
	case SHCLW_ERR_TOO_NEW: return "toonew";
	case SHCLW_ERR_UNSUPPORTED: return "unsupported";
	case SHCLW_ERR_AMBIGUOUS: return "ambiguous";
	case SHCLW_ERR_BACKUP_TAKEN: return "backuptaken";
	case SHCLW_ERR_NO_READ_BACK: return "noreadback";
	case SHCLW_ERR_CHANGED: return "changed";
	case SHCLW_ERR_EXISTS: return "exists";
	case SHCLW_ERR_NO_MEMORY: return "nomemory";
	}
	return "unknown";
}

const char *shclw_severity_name(shclw_severity s) { return s == SHCLW_SEV_ERROR ? "error" : "hint"; }

static void set_error(shclw_error *err, shclw_kind kind, const char *path, const char *why, int sys_errno) {
	if (!err) {
		return;
	}
	err->kind = kind;
	err->sys_errno = sys_errno;
	// A message cut short by the buffer is still a message, so snprintf's
	// truncation isn't an error here.
	int at = snprintf(err->msg, sizeof err->msg, "shclw: %s", shclw_kind_name(kind));
	if (at > 0 && path && (size_t)at < sizeof err->msg) {
		at += snprintf(err->msg + at, sizeof err->msg - (size_t)at, " \"%s\"", path);
	}
	if (at > 0 && why && (size_t)at < sizeof err->msg) {
		at += snprintf(err->msg + at, sizeof err->msg - (size_t)at, ": %s", why);
	}
	if (at > 0 && sys_errno && (size_t)at < sizeof err->msg) {
		(void)snprintf(err->msg + at, sizeof err->msg - (size_t)at, ": %s", strerror(sys_errno));
	}
}

// No branches, so clang's analyzer inlines it at any call depth and sees that
// a failure keeps its kind. Past its depth limit it guessed SHCLW_OK.
shclw_kind shclw_i_fail(shclw_error *err, shclw_kind kind, const char *path, const char *why, int sys_errno) {
	set_error(err, kind, path, why, sys_errno);
	return kind;
}

static void clear_error(shclw_error *err) {
	if (err) {
		err->kind = SHCLW_OK;
		err->sys_errno = 0;
		err->msg[0] = '\0';
	}
}

// No branches, as with shclw_i_fail.
shclw_kind shclw_i_ok(shclw_error *err) {
	clear_error(err);
	return SHCLW_OK;
}

void *shclw_i_alloc(size_t n) { return malloc(n ? n : 1); }

char *shclw_i_dup(const char *p, size_t n, size_t *len) {
	if (len) {
		*len = 0;
	}
	if (n == SIZE_MAX) {
		return NULL;
	}
	char *out = (char *)malloc(n + 1);
	if (!out) {
		return NULL;
	}
	if (n) {
		memcpy(out, p, n);
	}
	out[n] = '\0';
	if (len) {
		*len = n;
	}
	return out;
}

shclw_string *shclw_i_copy_strs(const shclw_string *items, size_t n, size_t *count) {
	*count = 0;
	if (n > (SIZE_MAX / 2) / sizeof(shclw_string)) {
		return NULL;
	}
	size_t text_bytes = 0;
	for (size_t i = 0; i < n; i++) {
		if (items[i].n >= SIZE_MAX / 4 - text_bytes) {
			return NULL;
		}
		text_bytes += items[i].n + 1;
	}
	shclw_string *out = (shclw_string *)shclw_i_alloc(n * sizeof(shclw_string) + text_bytes);
	if (!out) {
		return NULL;
	}
	char *text = (char *)(out + n);
	for (size_t i = 0; i < n; i++) {
		size_t len = items[i].n;
		if (len) {
			memcpy(text, items[i].p, len);
		}
		// The analyzer loses the sum above and takes the 1-byte allocation.
		text[len] = '\0'; // NOLINT(clang-analyzer-security.ArrayBound)
		out[i].p = text;
		out[i].n = len;
		text += len + 1;
	}
	*count = n;
	return out;
}

int64_t *shclw_i_copy_ints(const int64_t *items, size_t n, size_t *count) {
	*count = 0;
	if (n > SIZE_MAX / sizeof(int64_t)) {
		return NULL;
	}
	int64_t *out = (int64_t *)shclw_i_alloc(n * sizeof(int64_t));
	if (!out) {
		return NULL;
	}
	if (n) {
		memcpy(out, items, n * sizeof(int64_t));
	}
	*count = n;
	return out;
}

double *shclw_i_copy_floats(const double *items, size_t n, size_t *count) {
	*count = 0;
	if (n > SIZE_MAX / sizeof(double)) {
		return NULL;
	}
	double *out = (double *)calloc(n ? n : 1, sizeof(double));
	if (!out) {
		return NULL;
	}
	if (n) {
		memcpy(out, items, n * sizeof(double));
	}
	*count = n;
	return out;
}

size_t shclw_i_utf8_next(const char *s, size_t n, uint32_t *r) {
	const unsigned char *u = (const unsigned char *)s;
	if (n == 0) {
		return 0;
	}
	if (u[0] < 0x80) {
		*r = u[0];
		return 1;
	}
	// clang-format off
	static const struct { unsigned char lo, hi, mask; size_t size; uint32_t least; } leads[] = {
		{0xC2, 0xDF, 0x1F, 2, 0x80},
		{0xE0, 0xEF, 0x0F, 3, 0x800},
		{0xF0, 0xF4, 0x07, 4, 0x10000},
	};
	// clang-format on
	size_t size = 0;
	uint32_t c = 0;
	uint32_t least = 0;
	for (size_t i = 0; i < sizeof leads / sizeof leads[0]; i++) {
		if (u[0] >= leads[i].lo && u[0] <= leads[i].hi) {
			size = leads[i].size;
			c = u[0] & (uint32_t)leads[i].mask;
			least = leads[i].least;
		}
	}
	if (!size) {
		return 0;
	}
	if (n < size) {
		return 0;
	}
	for (size_t i = 1; i < size; i++) {
		if ((u[i] & 0xC0u) != 0x80u) {
			return 0;
		}
		c = (c << 6) | (u[i] & 0x3Fu);
	}
	if (c < least || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
		return 0;
	}
	*r = c;
	return size;
}

bool shclw_i_utf8_valid(const char *s, size_t n) {
	uint32_t r = 0;
	for (size_t i = 0; i < n;) {
		size_t size = shclw_i_utf8_next(s + i, n - i, &r);
		if (!size) {
			return false;
		}
		i += size;
	}
	return true;
}

void shclw_i_finite(shclw_got_float *g) {
	if (g->status == SHCLW_GOOD && !isfinite(g->value)) {
		g->status = SHCLW_BAD_TYPE;
		g->value = 0;
	}
}

void shclw_i_finite_all(shclw_got_floats *g) {
	if (g->status != SHCLW_GOOD || !g->value || !g->n) {
		return;
	}
	for (size_t i = 0; i < g->n; i++) {
		if (!isfinite(g->value[i])) {
			g->value[i] = 0;
			g->status = SHCLW_BAD_TYPE;
		}
	}
}

// The last digit up by one, carried, with the exponent moved when it carries
// out of the first.
static void digit_up(char *digits, size_t *n, int *exp) {
	size_t i = *n;
	while (i > 0 && digits[i - 1] == '9') {
		digits[--i] = '0';
	}
	if (i > 0) {
		digits[i - 1]++;
		return;
	}
	memmove(digits + 1, digits, *n);
	digits[0] = '1';
	digits[++*n] = '\0';
	++*exp;
}

// Whether digits are exact cut short where the rest is a 5 and only 0s, so
// the cut rounded a tie down.
static bool cut_at_tie(const char *exact, const char *digits, size_t n) {
	if (strlen(exact) <= n || exact[n] != '5' || strncmp(exact, digits, n) != 0) {
		return false;
	}
	return exact[n + 1 + strspn(exact + n + 1, "0")] == '\0';
}

// glibc's %.*e is the closest at a given length, which next to a power of 2
// can miss the interval that reads back while the digit above it is inside.
// So that one is tried too. An exact tie glibc rounds to even, and Go's
// Dragonbox does too, except at a power of 2, where it rounds up.
void shclw_i_shortest(double f, bool single, char *digits, int *exp) {
	char s[80];
	// 2^-1074 has 751 digits.
	char exact[800] = "";
	int e2 = 0;
	// Go leaves one power of 2 at even, the one it holds as its mantissa
	// times 2^-77: 2^-25, or as a float32, 2^-54.
	bool tie_up = f != 0 && frexp(fabs(f), &e2) == 0.5 && fabs(f) >= (single ? FLT_MIN : DBL_MIN) &&
	              e2 - 1 - (single ? 23 : 52) != -77;
	if (tie_up) {
		char all[820];
		(void)snprintf(all, sizeof all, "%.780e", fabs(f));
		size_t at = 0;
		for (const char *p = all; *p && *p != 'e'; p++) {
			if (*p >= '0' && *p <= '9') {
				exact[at++] = *p;
			}
		}
		while (at > 1 && exact[at - 1] == '0') {
			at--;
		}
		exact[at] = '\0';
	}
	for (int prec = 0; prec < (single ? 9 : 17); prec++) {
		(void)snprintf(s, sizeof s, "%.*e", prec, f);
		char *e = strchr(s, 'e');
		*exp = (int)strtol(e + 1, NULL, 10);
		size_t n = 0;
		for (char *p = s; p < e; p++) {
			if (*p >= '0' && *p <= '9') {
				digits[n++] = *p;
			}
		}
		digits[n] = '\0';
		if (exact[0] && cut_at_tie(exact, digits, n)) {
			digit_up(digits, &n, exp);
		}
		for (int pass = 0; pass < 2; pass++) {
			(void)snprintf(s, sizeof s, "%s%c.%se%d", signbit(f) ? "-" : "", digits[0], digits + 1, *exp);
			// codeql[cpp/equality-on-floats] Exact equality proves the text round-trips to the same binary value.
			if (single ? strtof(s, NULL) == (float)f : strtod(s, NULL) == f) {
				return;
			}
			digit_up(digits, &n, exp);
		}
	}
}

static unsigned char ascii_lower(unsigned char c) { return c >= 'A' && c <= 'Z' ? (unsigned char)(c + 32) : c; }

// By name without ASCII case, then by place in the list.
static int by_name(const void *a, const void *b) {
	const shclw_string *x = *(const shclw_string *const *)a;
	const shclw_string *y = *(const shclw_string *const *)b;
	for (size_t i = 0; i < x->n && i < y->n; i++) {
		unsigned char cx = ascii_lower((unsigned char)x->p[i]);
		unsigned char cy = ascii_lower((unsigned char)y->p[i]);
		if (cx != cy) {
			return cx < cy ? -1 : 1;
		}
	}
	if (x->n != y->n) {
		return x->n < y->n ? -1 : 1;
	}
	return x < y ? -1 : x > y;
}

shclw_string *shclw_i_dedupe_names(const shclw_string *names, size_t n, size_t *count) {
	*count = 0;
	if (n == 0 || n > SIZE_MAX / sizeof(shclw_string)) {
		return n ? NULL : shclw_i_copy_strs(names, 0, count);
	}
	const shclw_string **order = (const shclw_string **)malloc(n * sizeof(shclw_string *));
	bool *dup = (bool *)calloc(n, sizeof(bool));
	shclw_string *keep = (shclw_string *)malloc(n * sizeof(shclw_string));
	shclw_string *out = NULL;
	if (order && dup && keep) {
		for (size_t i = 0; i < n; i++) {
			order[i] = &names[i];
		}
		if (n > 1) {
			qsort((void *)order, n, sizeof order[0], by_name);
		}
		for (size_t i = 1; i < n; i++) {
			const shclw_string *x = order[i - 1];
			const shclw_string *y = order[i];
			bool same = x->n == y->n;
			for (size_t k = 0; same && k < x->n; k++) {
				same = ascii_lower((unsigned char)x->p[k]) == ascii_lower((unsigned char)y->p[k]);
			}
			// Sorted by place within a name, so the first is the one kept.
			if (same) {
				dup[y - names] = true;
			}
		}
		size_t kept = 0;
		for (size_t i = 0; i < n; i++) {
			if (!dup[i]) {
				keep[kept++] = names[i];
			}
		}
		out = shclw_i_copy_strs(keep, kept, count);
	}
	free((void *)order);
	free(dup);
	free(keep);
	return out;
}

shclw_node *shclw_i_copy_nodes(const shclw_node *nodes, size_t n, size_t *count) {
	*count = 0;
	if (n > (SIZE_MAX / 2) / sizeof(shclw_node)) {
		return NULL;
	}
	size_t bytes = n * sizeof(shclw_node);
	for (size_t i = 0; i < n; i++) {
		size_t more = nodes[i].path.n + nodes[i].value.n + 2;
		if (nodes[i].path.n >= SIZE_MAX / 8 || nodes[i].value.n >= SIZE_MAX / 8 || more >= SIZE_MAX / 2 - bytes) {
			return NULL;
		}
		bytes += more;
	}
	shclw_node *out = (shclw_node *)shclw_i_alloc(bytes);
	if (!out) {
		return NULL;
	}
	char *text = (char *)(out + n);
	for (size_t i = 0; i < n; i++) {
		const shclw_string *from[2] = {&nodes[i].path, &nodes[i].value};
		shclw_string *to[2] = {&out[i].path, &out[i].value};
		for (size_t k = 0; k < 2; k++) {
			if (from[k]->n) {
				memcpy(text, from[k]->p, from[k]->n);
			}
			// As in shclw_i_copy_strs, the analyzer loses the sum above.
			text[from[k]->n] = '\0'; // NOLINT(clang-analyzer-security.ArrayBound)
			to[k]->p = text;
			to[k]->n = from[k]->n;
			text += from[k]->n + 1;
		}
		out[i].line = nodes[i].line;
	}
	*count = n;
	return out;
}

shclw_diag *shclw_i_copy_diags(const shclw_diag *diags, size_t n, size_t *count) {
	*count = 0;
	if (n > (SIZE_MAX / 2) / sizeof(shclw_diag)) {
		return NULL;
	}
	size_t bytes = n * sizeof(shclw_diag);
	for (size_t i = 0; i < n; i++) {
		size_t more = diags[i].code.n + diags[i].message.n + 2;
		if (diags[i].code.n >= SIZE_MAX / 8 || diags[i].message.n >= SIZE_MAX / 8 || more >= SIZE_MAX / 2 - bytes) {
			return NULL;
		}
		bytes += more;
	}
	shclw_diag *out = (shclw_diag *)shclw_i_alloc(bytes);
	if (!out) {
		return NULL;
	}
	char *text = (char *)(out + n);
	for (size_t i = 0; i < n; i++) {
		const shclw_string *from[2] = {&diags[i].code, &diags[i].message};
		shclw_string *to[2] = {&out[i].code, &out[i].message};
		for (size_t k = 0; k < 2; k++) {
			if (from[k]->n) {
				memcpy(text, from[k]->p, from[k]->n);
			}
			text[from[k]->n] = '\0'; // NOLINT(clang-analyzer-security.ArrayBound)
			to[k]->p = text;
			to[k]->n = from[k]->n;
			text += from[k]->n + 1;
		}
		out[i].line = diags[i].line;
		out[i].layer = diags[i].layer;
		out[i].severity = diags[i].severity;
	}
	*count = n;
	return out;
}

// Go's InFileOrder sorts by this: the line, with a put's 0 after the rest,
// and Each order between equal lines.
static size_t line_key(const shclw_node *node) { return node->line ? node->line : SIZE_MAX; }

static char *pick(shclw_doc *d, const char *path, bool last, size_t *len) {
	size_t n = 0;
	shclw_node *nodes = shclw_each(d, path, &n);
	if (!nodes) {
		return NULL;
	}
	if (n == 0) {
		free(nodes);
		return shclw_i_dup(path, strlen(path), len);
	}
	size_t at = 0;
	for (size_t i = 1; i < n; i++) {
		size_t key = line_key(&nodes[i]);
		size_t best = line_key(&nodes[at]);
		if (last ? key >= best : key < best) {
			at = i;
		}
	}
	char *out = shclw_i_dup(nodes[at].path.p, nodes[at].path.n, len);
	free(nodes);
	return out;
}

char *shclw_first(shclw_doc *d, const char *path, size_t *len) { return pick(d, path, false, len); }
char *shclw_last(shclw_doc *d, const char *path, size_t *len) { return pick(d, path, true, len); }

static int by_line(const void *a, const void *b) {
	const shclw_node *x = *(const shclw_node *const *)a;
	const shclw_node *y = *(const shclw_node *const *)b;
	size_t kx = line_key(x);
	size_t ky = line_key(y);
	if (kx != ky) {
		return kx < ky ? -1 : 1;
	}
	// qsort isn't stable, so Each order breaks the tie.
	return x < y ? -1 : x > y;
}

size_t *shclw_lines(shclw_doc *d, const char *path, size_t *n) {
	*n = 0;
	size_t count = 0;
	shclw_node *nodes = shclw_each(d, path, &count);
	if (!nodes || count == 0) {
		free(nodes);
		return nodes ? (size_t *)shclw_i_alloc(sizeof(size_t)) : NULL;
	}
	const shclw_node **order = (const shclw_node **)malloc(count * sizeof(shclw_node *));
	size_t *out = (size_t *)malloc(count * sizeof(size_t));
	if (!order || !out) {
		free((void *)order);
		free(out);
		free(nodes);
		return NULL;
	}
	for (size_t i = 0; i < count; i++) {
		order[i] = &nodes[i];
	}
	if (count > 1) {
		qsort((void *)order, count, sizeof order[0], by_line);
	}
	for (size_t i = 0; i < count; i++) {
		out[i] = order[i]->line;
	}
	free((void *)order);
	free(nodes);
	*n = count;
	return out;
}

static shclw_kind not_finite(shclw_error *err, const char *path) {
	return shclw_i_fail(err, SHCLW_ERR_BAD_VALUE, path, "not a finite number", 0);
}

static bool same_strs(const shclw_string *a, const shclw_string *b, size_t n) {
	for (size_t i = 0; i < n; i++) {
		if (a[i].n != b[i].n || (a[i].n && memcmp(a[i].p, b[i].p, a[i].n) != 0)) {
			return false;
		}
	}
	return true;
}

static bool same_floats(const double *a, const double *b, size_t n) {
	for (size_t i = 0; i < n; i++) {
		// codeql[cpp/equality-on-floats] Exact equality compares parsed numeric values without a tolerance.
		if (a[i] != b[i]) {
			return false;
		}
	}
	return true;
}

// Go's VerifyPut: shcl can take a value and read back something else. See
// "What the wrapper evens out" in design.md.
shclw_kind shclw_i_verify_put(shclw_doc *d, const char *path, const shclw_i_value *v, shclw_error *err) {
	shclw_status st = SHCLW_GOOD;
	bool same = false;
	bool empty_ok = false;
	bool oom = false;
	switch (v->kind) {
	case SHCLW_I_INT: {
		shclw_got_int g = shclw_get_int(d, path);
		st = g.status;
		same = g.value == v->i;
		break;
	}
	case SHCLW_I_FLOAT: {
		shclw_got_float g = shclw_get_float(d, path);
		st = g.status;
		// codeql[cpp/equality-on-floats] Exact equality verifies the value read back after writing.
		same = g.value == v->f;
		break;
	}
	case SHCLW_I_BOOL: {
		shclw_got_bool g = shclw_get_bool(d, path);
		st = g.status;
		same = g.value == v->b;
		break;
	}
	case SHCLW_I_STR: {
		shclw_got_str g = shclw_get_str(d, path);
		st = g.status;
		oom = !g.value;
		same = g.value && g.len == v->n && (!v->n || !memcmp(g.value, v->str, v->n));
		free(g.value);
		break;
	}
	case SHCLW_I_STRS: {
		shclw_got_strs g = shclw_get_strs(d, path);
		st = g.status;
		oom = !g.value;
		same = g.value && g.n == v->n && same_strs(g.value, v->strs, v->n);
		empty_ok = true;
		free(g.value);
		break;
	}
	case SHCLW_I_INTS: {
		shclw_got_ints g = shclw_get_ints(d, path);
		st = g.status;
		oom = !g.value;
		same = g.value && g.n == v->n && (!v->n || !memcmp(g.value, v->ints, v->n * sizeof(int64_t)));
		empty_ok = true;
		free(g.value);
		break;
	}
	case SHCLW_I_FLOATS: {
		shclw_got_floats g = shclw_get_floats(d, path);
		st = g.status;
		oom = !g.value;
		same = g.value && g.n == v->n && same_floats(g.value, v->floats, v->n);
		empty_ok = true;
		free(g.value);
		break;
	}
	}
	if (oom) {
		return shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
	}
	// 1.2 and 2.0 read an empty array back as Empty, and can't write one any
	// other way.
	if ((st == SHCLW_GOOD && same) || (empty_ok && st == SHCLW_EMPTY && v->n == 0)) {
		return shclw_i_ok(err);
	}
	char why[64];
	(void)snprintf(why, sizeof why, "reads back as %s", st == SHCLW_GOOD ? "a different value" : shclw_status_name(st));
	return shclw_i_fail(err, SHCLW_ERR_NO_READ_BACK, path, why, 0);
}

shclw_i_value shclw_i_value_of(shclw_i_kind kind, size_t n) {
	shclw_i_value v = {kind, 0, 0, false, NULL, NULL, NULL, NULL, n};
	return v;
}

static shclw_kind put(shclw_doc *d, const char *path, const shclw_i_value *v, shclw_error *err) {
	shclw_doc *top = shclw_i_top(d);
	shclw_kind k = shclw_i_set(top, path, v, err);
	if (k == SHCLW_OK) {
		k = shclw_i_verify_put(top, path, v, err);
	}
	// The set is in place even when the read-back failed.
	shclw_kind m = shclw_i_edited(d, NULL);
	return m == SHCLW_OK ? k : shclw_i_fail(err, m, path, NULL, 0);
}

shclw_kind shclw_put_int(shclw_doc *d, const char *path, int64_t v, shclw_error *err) {
	shclw_i_value val = shclw_i_value_of(SHCLW_I_INT, 0);
	val.i = v;
	return put(d, path, &val, err);
}

shclw_kind shclw_put_float(shclw_doc *d, const char *path, double v, shclw_error *err) {
	if (!isfinite(v)) {
		return not_finite(err, path);
	}
	shclw_i_value val = shclw_i_value_of(SHCLW_I_FLOAT, 0);
	val.f = v;
	return put(d, path, &val, err);
}

// (double)v would write the float's binary noise: 0.9 as 0.8999999761581421.
shclw_kind shclw_put_float32(shclw_doc *d, const char *path, float v, shclw_error *err) {
	if (!isfinite(v)) {
		return not_finite(err, path);
	}
	char digits[40];
	int exp = 0;
	shclw_i_shortest((double)v, true, digits, &exp);
	char text[64];
	(void)snprintf(text, sizeof text, "%s%c.%se%d", signbit(v) ? "-" : "", digits[0], digits + 1, exp);
	return shclw_put_float(d, path, strtod(text, NULL), err);
}

shclw_kind shclw_put_bool(shclw_doc *d, const char *path, bool v, shclw_error *err) {
	shclw_i_value val = shclw_i_value_of(SHCLW_I_BOOL, 0);
	val.b = v;
	return put(d, path, &val, err);
}

shclw_kind shclw_put_str(shclw_doc *d, const char *path, const char *v, size_t len, shclw_error *err) {
	shclw_i_value val = shclw_i_value_of(SHCLW_I_STR, v ? len : 0);
	val.str = v ? v : "";
	return put(d, path, &val, err);
}

shclw_kind shclw_put_strs(shclw_doc *d, const char *path, const shclw_string *v, size_t n, shclw_error *err) {
	shclw_i_value val = shclw_i_value_of(SHCLW_I_STRS, v ? n : 0);
	val.strs = v;
	return put(d, path, &val, err);
}

shclw_kind shclw_put_ints(shclw_doc *d, const char *path, const int64_t *v, size_t n, shclw_error *err) {
	shclw_i_value val = shclw_i_value_of(SHCLW_I_INTS, v ? n : 0);
	val.ints = v;
	return put(d, path, &val, err);
}

shclw_kind shclw_put_floats(shclw_doc *d, const char *path, const double *v, size_t n, shclw_error *err) {
	for (size_t i = 0; v && i < n; i++) {
		if (!isfinite(v[i])) {
			return not_finite(err, path);
		}
	}
	shclw_i_value val = shclw_i_value_of(SHCLW_I_FLOATS, v ? n : 0);
	val.floats = v;
	return put(d, path, &val, err);
}

shclw_kind shclw_i_verify_syntax(shclw_doc *d, const char *path, shclw_doc *alone, shclw_doc *line, shclw_error *err) {
	size_t a_len = 0;
	size_t l_len = 0;
	char *a = shclw_canonical(alone, &a_len);
	char *l = shclw_canonical(line, &l_len);
	bool oom = !a || !l;
	bool typed = !oom && a_len == l_len && !memcmp(a, l, a_len);
	free(a);
	free(l);
	if (oom) {
		return shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
	}
	if (!typed) {
		return shclw_i_fail(err, SHCLW_ERR_NO_READ_BACK, path, "the text doesn't read back as typed", 0);
	}
	shclw_got_str got = shclw_get_str(d, path);
	shclw_got_str want = shclw_get_str(line, "k");
	oom = !got.value || !want.value;
	bool same = !oom && got.status == want.status && got.len == want.len && !memcmp(got.value, want.value, got.len);
	free(got.value);
	free(want.value);
	if (oom) {
		return shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
	}
	if (!same) {
		return shclw_i_fail(err, SHCLW_ERR_NO_READ_BACK, path, "reads back as a different value", 0);
	}
	return shclw_i_ok(err);
}

shclw_kind shclw_read_text(const char *path, char **text, size_t *len, shclw_error *err) {
	*text = NULL;
	*len = 0;
	FILE *f = fopen(path, "rb");
	if (!f) {
		int e = errno;
		return shclw_i_fail(err, e == ENOENT ? SHCLW_ERR_NOT_FOUND : SHCLW_ERR_IO, path, NULL, e);
	}
	size_t cap = 4096;
	size_t n = 0;
	char *buf = (char *)malloc(cap);
	shclw_kind kind = SHCLW_OK;
	int e = 0;
	while (buf) {
		if (cap - n < 2) {
			char *grown = cap <= SIZE_MAX / 2 ? (char *)realloc(buf, cap * 2) : NULL;
			if (!grown) {
				free(buf);
				buf = NULL;
				break;
			}
			buf = grown;
			cap *= 2;
		}
		size_t want = cap - n - 1;
		size_t got = fread(buf + n, 1, want, f);
		n += got;
		if (got < want) {
			if (ferror(f)) {
				e = errno;
				kind = SHCLW_ERR_IO;
			}
			break;
		}
	}
	if (fclose(f) != 0 && kind == SHCLW_OK && buf) {
		e = errno;
		kind = SHCLW_ERR_IO;
	}
	if (!buf) {
		return shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
	}
	if (kind != SHCLW_OK) {
		free(buf);
		return shclw_i_fail(err, kind, path, NULL, e);
	}
	if (!shclw_i_utf8_valid(buf, n)) {
		free(buf);
		return shclw_i_fail(err, SHCLW_ERR_IO, path, "not valid UTF-8", 0);
	}
	buf[n] = '\0';
	*text = buf;
	*len = n;
	return shclw_i_ok(err);
}

// Whether path still holds was, byte for byte.
static bool unchanged(const char *path, const char *was, size_t was_len) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	char chunk[4096];
	size_t at = 0;
	bool same = true;
	for (;;) {
		size_t got = fread(chunk, 1, sizeof chunk, f);
		if (got > was_len - at || (got && memcmp(chunk, was + at, got) != 0)) {
			same = false;
			break;
		}
		at += got;
		if (got < sizeof chunk) {
			same = !ferror(f) && at == was_len;
			break;
		}
	}
	return fclose(f) == 0 && same;
}

bool shclw_i_missing(const char *path) {
#ifdef _WIN32
	struct _stat64 st;
	return _stat64(path, &st) != 0 && errno == ENOENT;
#else
	struct stat st;
	return lstat(path, &st) != 0 && errno == ENOENT;
#endif
}

// A copy of what's still at path.
static void drop_backup(char *name) {
	if (name) {
		(void)remove(name);
		free(name);
	}
}

static void note(shclw_error *err, const char *what, const char *name) {
	if (!err) {
		return;
	}
	size_t at = strlen(err->msg);
	if (at < sizeof err->msg) {
		(void)snprintf(err->msg + at, sizeof err->msg - at, "; %s%s", what, name ? name : "");
	}
}

shclw_kind shclw_i_replace_read(const char *path, const char *was, size_t was_len, const char *data, size_t len,
                                const shclw_i_replacer *how, char **backup_name, shclw_error *err) {
	static const char changed[] = "changed since it was read; nothing written";
	*backup_name = NULL;
	if (!unchanged(path, was, was_len)) {
		return shclw_i_fail(err, SHCLW_ERR_CHANGED, path, changed, 0);
	}
	char *name = NULL;
	if (how->backup) {
		shclw_kind k = how->backup(how->ctx, &name, err);
		if (k != SHCLW_OK) {
			return k;
		}
	}
	if (!unchanged(path, was, was_len)) {
		drop_backup(name);
		return shclw_i_fail(err, SHCLW_ERR_CHANGED, path, changed, 0);
	}
	shclw_kind k = how->write(how->ctx, path, data, len, err);
	if (k == SHCLW_OK) {
		*backup_name = name;
		return k;
	}
	if (unchanged(path, was, was_len)) {
		drop_backup(name);
		return k;
	}
	if (!name) {
		return k;
	}
	// A replace that fails part way on Windows can leave nothing at path.
	if (shclw_i_missing(path) && rename(name, path) == 0) {
		note(err, "the original was put back", NULL);
	} else {
		note(err, "the original is ", name);
	}
	free(name);
	return k;
}

static shclw_upgraded *upgraded_block(const char *text, size_t len, int64_t from, bool changed, size_t unsure,
                                      const char *backup, const shclw_diag *diags, size_t n_diags, const size_t *lost,
                                      size_t n_lost, const size_t *rewritten, size_t n_rewritten) {
	size_t blen = backup ? strlen(backup) : 0;
	if (len >= SIZE_MAX / 4 || blen >= SIZE_MAX / 4 || n_diags > (SIZE_MAX / 4) / sizeof(shclw_diag) ||
	    n_lost > (SIZE_MAX / 4) / sizeof(size_t) || n_rewritten > (SIZE_MAX / 4) / sizeof(size_t)) {
		return NULL;
	}
	// The diagnostics go through their own block first, since its layout is
	// shclw_i_copy_diags' to keep.
	size_t n = 0;
	shclw_diag *ds = shclw_i_copy_diags(diags, n_diags, &n);
	if (!ds) {
		return NULL;
	}
	// The line lists sit after the diagnostics. shclw_diag holds size_t
	// fields, so its end is aligned for them.
	size_t head = sizeof(shclw_upgraded) + n * sizeof(shclw_diag) + (n_lost + n_rewritten) * sizeof(size_t);
	size_t strings = 0;
	for (size_t i = 0; i < n; i++) {
		strings += ds[i].code.n + ds[i].message.n + 2;
	}
	// Not a ?:, as in shclw_i_merges_of.
	shclw_upgraded *u = NULL;
	if (strings < SIZE_MAX / 4) {
		u = (shclw_upgraded *)malloc(head + strings + len + blen + 2);
	}
	if (!u) {
		free(ds);
		return NULL;
	}
	shclw_diag *to = (shclw_diag *)(u + 1);
	char *at = (char *)u + head;
	for (size_t i = 0; i < n; i++) {
		to[i] = ds[i];
		memcpy(at, ds[i].code.p, ds[i].code.n + 1);
		to[i].code.p = at;
		at += ds[i].code.n + 1;
		memcpy(at, ds[i].message.p, ds[i].message.n + 1);
		to[i].message.p = at;
		at += ds[i].message.n + 1;
	}
	free(ds);
	if (len) {
		memcpy(at, text, len);
	}
	at[len] = '\0';
	u->text.p = at;
	u->text.n = len;
	at += len + 1;
	if (blen) {
		memcpy(at, backup, blen);
	}
	at[blen] = '\0';
	u->backup.p = at;
	u->backup.n = blen;
	u->from = from;
	u->changed = changed;
	u->unsure = unsure;
	u->diags = n ? to : NULL;
	u->n_diags = n;
	size_t *lines = (size_t *)(to + n);
	u->lost = n_lost ? lines : NULL;
	u->n_lost = n_lost;
	if (n_lost) {
		memcpy(lines, lost, n_lost * sizeof(size_t));
	}
	u->rewritten = n_rewritten ? lines + n_lost : NULL;
	u->n_rewritten = n_rewritten;
	if (n_rewritten) {
		memcpy(lines + n_lost, rewritten, n_rewritten * sizeof(size_t));
	}
	return u;
}

shclw_upgraded *shclw_i_upgraded(const char *text, size_t len, int64_t from, bool changed, size_t unsure,
                                 const char *backup, const shclw_diag *diags, size_t n_diags) {
	return upgraded_block(text, len, from, changed, unsure, backup, diags, n_diags, NULL, 0, NULL, 0);
}

shclw_upgraded *shclw_i_upgraded_lines(const shclw_upgraded *u, const char *backup, const size_t *lost, size_t n_lost,
                                       const size_t *rewritten, size_t n_rewritten) {
	return upgraded_block(u->text.p, u->text.n, u->from, u->changed, u->unsure, backup ? backup : u->backup.p, u->diags,
	                      u->n_diags, lost, n_lost, rewritten, n_rewritten);
}

static int by_text(const void *a, const void *b) {
	const shclw_string *x = (const shclw_string *)a, *y = (const shclw_string *)b;
	size_t n = x->n < y->n ? x->n : y->n;
	int c = n ? memcmp(x->p, y->p, n) : 0;
	return c ? c : (x->n > y->n) - (x->n < y->n);
}

// text split on LF, keeping any CR, so a line whose ending changed counts as
// written differently. False only when memory ran out.
static bool text_lines(const char *text, size_t len, shclw_string **lines, size_t *n) {
	*lines = NULL;
	*n = 0;
	if (!len) {
		return true;
	}
	size_t count = 1;
	for (size_t i = 0; i + 1 < len; i++) {
		count += text[i] == '\n';
	}
	shclw_string *out = (shclw_string *)shclw_i_alloc(count * sizeof(shclw_string));
	if (!out) {
		return false;
	}
	const char *start = text, *end = text + len;
	if (end[-1] == '\n') {
		end--;
	}
	for (size_t k = 0; k < count; k++) {
		const char *nl = (const char *)memchr(start, '\n', (size_t)(end - start));
		const char *stop = nl ? nl : end;
		out[k].p = start;
		out[k].n = (size_t)(stop - start);
		start = stop + 1;
	}
	*lines = out;
	*n = count;
	return true;
}

bool shclw_i_rewritten_lines(const char *was, size_t was_len, const char *now, size_t now_len, const size_t *lost,
                             size_t n_lost, size_t **out, size_t *n) {
	*out = NULL;
	*n = 0;
	size_t n_was = 0, n_now = 0;
	shclw_string *olds = NULL, *news = NULL;
	bool ok = text_lines(was, was_len, &olds, &n_was);
	ok = text_lines(now, now_len, &news, &n_now) && ok;
	// taken[k] counts the matches used from the run of equal lines at k
	size_t *taken = (size_t *)calloc(n_now ? n_now : 1, sizeof(size_t));
	size_t *list = (size_t *)shclw_i_alloc((n_was ? n_was : 1) * sizeof(size_t));
	if (!ok || !taken || !list) {
		free(olds);
		free(news);
		free(taken);
		free(list);
		return false;
	}
	if (n_now) {
		qsort(news, n_now, sizeof(shclw_string), by_text);
	}
	size_t got = 0, at_lost = 0;
	for (size_t i = 0; i < n_was; i++) {
		while (at_lost < n_lost && lost[at_lost] < i + 1) {
			at_lost++;
		}
		if (at_lost < n_lost && lost[at_lost] == i + 1) {
			continue;
		}
		// the first of the run of equal lines
		size_t lo = 0, hi = n_now;
		while (lo < hi) {
			size_t mid = lo + (hi - lo) / 2;
			if (by_text(&news[mid], &olds[i]) < 0) {
				lo = mid + 1;
			} else {
				hi = mid;
			}
		}
		size_t next = lo + (lo < n_now ? taken[lo] : 0);
		if (next < n_now && by_text(&news[next], &olds[i]) == 0) {
			taken[lo]++;
			continue;
		}
		list[got++] = i + 1;
	}
	free(olds);
	free(news);
	free(taken);
	if (!got) {
		free(list);
		return true;
	}
	*out = list;
	*n = got;
	return true;
}

shclw_i_file_id shclw_i_file_id_of(const char *path) {
	shclw_i_file_id id = {false, 0, 0};
#ifndef _WIN32
	struct stat st;
	if (stat(path, &st) == 0) {
		id.ok = true;
		id.dev = (uint64_t)st.st_dev;
		id.ino = (uint64_t)st.st_ino;
	}
#else
	(void)path;
#endif
	return id;
}

bool shclw_i_same_file(const char *path, const char *was, shclw_i_file_id id) {
#ifdef _WIN32
	char a[_MAX_PATH], b[_MAX_PATH];
	if (_fullpath(a, path, sizeof a) && _fullpath(b, was, sizeof b)) {
		return _stricmp(a, b) == 0;
	}
	(void)id;
	return strcmp(path, was) == 0;
#else
	if (strcmp(path, was) == 0) {
		return true;
	}
	shclw_i_file_id now = shclw_i_file_id_of(path);
	return id.ok && now.ok && now.dev == id.dev && now.ino == id.ino;
#endif
}

bool shclw_i_irregular(const char *path) {
#ifdef _WIN32
	struct _stat64 st;
	return _stat64(path, &st) == 0 && (st.st_mode & _S_IFMT) != _S_IFREG;
#else
	struct stat st;
	return stat(path, &st) == 0 && !S_ISREG(st.st_mode);
#endif
}

static int by_spot(const void *a, const void *b) {
	const shclw_spot *x = (const shclw_spot *)a;
	const shclw_spot *y = (const shclw_spot *)b;
	if (x->layer != y->layer) {
		return x->layer < y->layer ? -1 : 1;
	}
	return (x->line > y->line) - (x->line < y->line);
}

shclw_spot *shclw_i_unused_of(const shclw_diag *diags, size_t n, size_t *count) {
	*count = 0;
	shclw_spot *out = (shclw_spot *)calloc(n + 1, sizeof(shclw_spot));
	if (!out) {
		return NULL;
	}
	size_t k = 0;
	for (size_t i = 0; i < n; i++) {
		if (diags[i].severity == SHCLW_SEV_ERROR && diags[i].line > 0) {
			out[k].line = diags[i].line;
			out[k].layer = diags[i].layer;
			k++;
		}
	}
	qsort(out, k, sizeof *out, by_spot);
	size_t w = 0;
	for (size_t i = 0; i < k; i++) {
		if (w == 0 || by_spot(&out[w - 1], &out[i]) != 0) {
			out[w++] = out[i];
		}
	}
	*count = w;
	return out;
}

// One merge as it's found, before shclw_i_merges_of packs them in one block.
typedef struct {
	char *path;
	size_t path_len;
	size_t *lines;
	size_t n_lines, cap;
	size_t layer;
} merge_acc;

// A path still to walk, and in the probe walk the path in the loaded document
// of the node it stands for.
typedef struct {
	char *path;
	char *orig;
} walk_item;

typedef struct {
	shclw_doc *d, *p;
	// By line: the deepest node's path in d, and whether the probe renamed it.
	char **at;
	bool *folded;
	size_t n_lines;
	size_t layer;
	merge_acc *acc;
	size_t n_acc, cap_acc;
	walk_item *stack;
	size_t n_stack, cap_stack;
	bool oom;
} merge_ctx;

static void push_walk(merge_ctx *c, const char *path, size_t path_len, const char *orig) {
	if (c->n_stack == c->cap_stack) {
		size_t cap = c->cap_stack ? c->cap_stack * 2 : 16;
		walk_item *grown =
			cap < SIZE_MAX / sizeof(walk_item) ? (walk_item *)realloc(c->stack, cap * sizeof(walk_item)) : NULL;
		if (!grown) {
			c->oom = true;
			return;
		}
		c->stack = grown;
		c->cap_stack = cap;
	}
	walk_item *w = &c->stack[c->n_stack];
	w->path = shclw_i_dup(path, path_len, NULL);
	w->orig = orig ? shclw_i_dup(orig, strlen(orig), NULL) : NULL;
	if (!w->path || (orig && !w->orig)) {
		free(w->path);
		free(w->orig);
		c->oom = true;
		return;
	}
	c->n_stack++;
}

static bool push_line(merge_acc *m, size_t line) {
	if (m->n_lines == m->cap) {
		size_t cap = m->cap ? m->cap * 2 : 4;
		size_t *grown = cap < SIZE_MAX / sizeof(size_t) ? (size_t *)realloc(m->lines, cap * sizeof(size_t)) : NULL;
		if (!grown) {
			return false;
		}
		m->lines = grown;
		m->cap = cap;
	}
	m->lines[m->n_lines++] = line;
	return true;
}

static void add_merge(merge_ctx *c, const shclw_node *kept, size_t line) {
	merge_acc *m = NULL;
	for (size_t i = 0; i < c->n_acc && !m; i++) {
		merge_acc *x = &c->acc[i];
		if (x->layer == c->layer && x->path_len == kept->path.n && memcmp(x->path, kept->path.p, x->path_len) == 0) {
			m = x;
		}
	}
	if (!m) {
		if (c->n_acc == c->cap_acc) {
			size_t cap = c->cap_acc ? c->cap_acc * 2 : 8;
			merge_acc *grown =
				cap < SIZE_MAX / sizeof(merge_acc) ? (merge_acc *)realloc(c->acc, cap * sizeof(merge_acc)) : NULL;
			if (!grown) {
				c->oom = true;
				return;
			}
			c->acc = grown;
			c->cap_acc = cap;
		}
		m = &c->acc[c->n_acc];
		memset(m, 0, sizeof *m);
		m->layer = c->layer;
		m->path = shclw_i_dup(kept->path.p, kept->path.n, &m->path_len);
		if (!m->path) {
			c->oom = true;
			return;
		}
		c->n_acc++;
		if (!push_line(m, kept->line)) {
			c->oom = true;
			return;
		}
	}
	c->oom = c->oom || !push_line(m, line);
}

// The nodes under path in x, one block per name, through see.
typedef void (*see_fn)(merge_ctx *c, const walk_item *from, const shclw_node *n, shclw_string name);

static void walk_from(merge_ctx *c, shclw_doc *x, see_fn see) {
	while (c->n_stack > 0 && !c->oom) {
		c->n_stack--;
		char *path = c->stack[c->n_stack].path;
		char *orig = c->stack[c->n_stack].orig;
		const walk_item from = {path, orig};
		size_t k = 0;
		shclw_string *names = shclw_names(x, path, &k);
		c->oom = c->oom || !names;
		for (size_t i = 0; names && i < k && !c->oom; i++) {
			char *child = shclw_child(x, path, names[i].p, names[i].n, NULL);
			size_t m = 0;
			shclw_node *nodes = child ? shclw_each(x, child, &m) : NULL;
			c->oom = c->oom || !nodes;
			for (size_t j = 0; nodes && j < m && !c->oom; j++) {
				see(c, &from, &nodes[j], names[i]);
			}
			free(nodes);
			free(child);
		}
		free(names);
		free(path);
		free(orig);
	}
}

static void see_loaded(merge_ctx *c, const walk_item *from, const shclw_node *n, shclw_string name) {
	(void)from;
	(void)name;
	size_t line = n->line;
	if (line > 0 && line <= c->n_lines && (!c->at[line] || strlen(c->at[line]) < n->path.n)) {
		free(c->at[line]);
		c->at[line] = shclw_i_dup(n->path.p, n->path.n, NULL);
		c->oom = c->oom || !c->at[line];
	}
	push_walk(c, n->path.p, n->path.n, NULL);
}

static int probe_name(size_t line, char *buf, size_t size) { return snprintf(buf, size, "shclwprobe%zux", line); }

// Whether a node in the probe has a child on its own line, as a dotted line's
// steps do.
static bool on_own_line(merge_ctx *c, const shclw_node *n) {
	size_t k = 0;
	shclw_string *names = shclw_names(c->p, n->path.p, &k);
	bool found = false;
	c->oom = c->oom || !names;
	for (size_t i = 0; names && i < k && !found && !c->oom; i++) {
		char *child = shclw_child(c->p, n->path.p, names[i].p, names[i].n, NULL);
		size_t m = 0;
		shclw_node *kids = child ? shclw_each(c->p, child, &m) : NULL;
		c->oom = c->oom || !kids;
		for (size_t j = 0; kids && j < m; j++) {
			found = found || kids[j].line == n->line;
		}
		free(kids);
		free(child);
	}
	free(names);
	return found;
}

static void see_probe(merge_ctx *c, const walk_item *from, const shclw_node *n, shclw_string name) {
	size_t line = n->line;
	bool in = line > 0 && line <= c->n_lines;
	if (!in || !c->folded[line]) {
		if (in && c->at[line]) {
			push_walk(c, n->path.p, n->path.n, c->at[line]);
		}
		return;
	}
	char pre[48];
	int pn = probe_name(line, pre, sizeof pre);
	size_t pl = pn > 0 ? (size_t)pn : 0;
	bool prefixed = name.n >= pl;
	for (size_t i = 0; prefixed && i < pl; i++) {
		char ch = name.p[i];
		prefixed = (ch >= 'A' && ch <= 'Z' ? (char)(ch - 'A' + 'a') : ch) == pre[i];
	}
	if (prefixed) {
		name.p += pl;
		name.n -= pl;
	}
	char *child = shclw_child(c->d, from->orig, name.p, name.n, NULL);
	size_t m = 0;
	shclw_node *ks = child ? shclw_each(c->d, child, &m) : NULL;
	c->oom = c->oom || !ks;
	const shclw_node *kept = NULL;
	for (size_t i = 0; ks && i < m; i++) {
		const shclw_node *k = &ks[i];
		bool same = k->value.n == n->value.n && (k->value.n == 0 || memcmp(k->value.p, n->value.p, k->value.n) == 0);
		if (same && k->line > 0 && k->line < line && (!kept || k->line > kept->line)) {
			kept = k;
		}
	}
	if (kept && !c->oom) {
		// A dotted line folds each step; only its last one is the copy.
		if (!on_own_line(c, n)) {
			add_merge(c, kept, line);
		}
		push_walk(c, n->path.p, n->path.n, kept->path.p);
	}
	free(ks);
	free(child);
}

// The probe: each line on no node, with no error, that starts with a name has
// the name made unique, so a second parse keeps that copy apart.
static char *probe_text(merge_ctx *c, const char *text, size_t len, const bool *bad, size_t *out_len,
                        size_t *n_folded) {
	*out_len = 0;
	*n_folded = 0;
	if (c->n_lines >= (SIZE_MAX - len) / 48) {
		return NULL;
	}
	char *out = (char *)malloc(len + c->n_lines * 48 + 1);
	if (!out) {
		return NULL;
	}
	size_t w = 0;
	size_t pos = 0;
	for (size_t line = 1; line <= c->n_lines; line++) {
		const char *nl = pos < len ? (const char *)memchr(text + pos, '\n', len - pos) : NULL;
		size_t end = nl ? (size_t)(nl - text) : len;
		size_t body = pos;
		while (body < end && (text[body] == ' ' || text[body] == '\t')) {
			body++;
		}
		unsigned char ch = body < end ? (unsigned char)text[body] : 0;
		bool quote = ch == '"' || ch == '\'';
		bool letter = ch >= 0x80 || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
		size_t cut = quote ? body + 1 : body;
		memcpy(out + w, text + pos, cut - pos);
		w += cut - pos;
		if (!c->at[line] && !bad[line] && (quote || letter)) {
			int pn = probe_name(line, out + w, 48);
			w += pn > 0 ? (size_t)pn : 0;
			c->folded[line] = true;
			(*n_folded)++;
		}
		memcpy(out + w, text + cut, end - cut);
		w += end - cut;
		if (end < len) {
			out[w++] = '\n';
		}
		pos = end + 1;
	}
	out[w] = '\0';
	*out_len = w;
	return out;
}

static void merges_in(merge_ctx *c, const shclw_i_source *src) {
	c->n_lines = 1;
	for (size_t i = 0; i < src->len; i++) {
		c->n_lines += src->text[i] == '\n';
	}
	c->at = (char **)calloc(c->n_lines + 1, sizeof(char *));
	c->folded = (bool *)calloc(c->n_lines + 1, sizeof(bool));
	bool *bad = (bool *)calloc(c->n_lines + 1, sizeof(bool));
	c->d = shclw_parse(src->text, src->len);
	c->oom = !c->at || !c->folded || !bad || !c->d;
	if (!c->oom) {
		push_walk(c, "", 0, NULL);
		walk_from(c, c->d, see_loaded);
	}
	size_t nd = 0;
	shclw_diag *ds = c->oom ? NULL : shclw_diags(c->d, &nd);
	c->oom = c->oom || !ds;
	for (size_t i = 0; ds && i < nd; i++) {
		if (ds[i].severity == SHCLW_SEV_ERROR && ds[i].line <= c->n_lines) {
			bad[ds[i].line] = true;
		}
	}
	free(ds);
	size_t plen = 0;
	size_t n_folded = 0;
	char *probe = c->oom ? NULL : probe_text(c, src->text, src->len, bad, &plen, &n_folded);
	c->oom = c->oom || !probe;
	if (!c->oom && n_folded > 0) {
		c->p = shclw_parse(probe, plen);
		c->oom = !c->p;
		if (!c->oom) {
			push_walk(c, "", 0, "");
			walk_from(c, c->p, see_probe);
		}
	}
	while (c->n_stack > 0) {
		c->n_stack--;
		free(c->stack[c->n_stack].path);
		free(c->stack[c->n_stack].orig);
	}
	free(probe);
	free(bad);
	for (size_t i = 0; c->at && i <= c->n_lines; i++) {
		free(c->at[i]);
	}
	free((void *)c->at);
	free(c->folded);
	shclw_free(c->d);
	shclw_free(c->p);
	c->at = NULL;
	c->folded = NULL;
	c->d = NULL;
	c->p = NULL;
}

static int by_merge(const void *a, const void *b) {
	const merge_acc *x = (const merge_acc *)a;
	const merge_acc *y = (const merge_acc *)b;
	if (x->layer != y->layer) {
		return x->layer < y->layer ? -1 : 1;
	}
	if (x->lines[0] != y->lines[0]) {
		return x->lines[0] < y->lines[0] ? -1 : 1;
	}
	size_t n = x->path_len < y->path_len ? x->path_len : y->path_len;
	int r = memcmp(x->path, y->path, n);
	return r ? r : (x->path_len > y->path_len) - (x->path_len < y->path_len);
}

static int by_size_t(const void *a, const void *b) {
	size_t x = *(const size_t *)a;
	size_t y = *(const size_t *)b;
	return (x > y) - (x < y);
}

shclw_merge *shclw_i_merges_of(const shclw_i_source *srcs, size_t n_srcs, size_t *count) {
	*count = 0;
	merge_ctx c;
	memset(&c, 0, sizeof c);
	for (size_t i = 0; i < n_srcs && !c.oom; i++) {
		c.layer = srcs[i].layer;
		merges_in(&c, &srcs[i]);
	}
	size_t bytes = 0;
	bool ok = !c.oom && c.n_acc < SIZE_MAX / 8 / sizeof(shclw_merge);
	if (ok) {
		bytes = c.n_acc * sizeof(shclw_merge);
		for (size_t i = 0; ok && i < c.n_acc; i++) {
			qsort(c.acc[i].lines, c.acc[i].n_lines, sizeof(size_t), by_size_t);
			ok = c.acc[i].n_lines < SIZE_MAX / 8 / sizeof(size_t) && c.acc[i].path_len < SIZE_MAX / 8;
			bytes += c.acc[i].n_lines * sizeof(size_t);
		}
	}
	size_t chars = 0;
	for (size_t i = 0; ok && i < c.n_acc; i++) {
		chars += c.acc[i].path_len + 1;
		ok = chars < SIZE_MAX / 4 && bytes < SIZE_MAX / 4;
	}
	if (ok && c.n_acc > 1) {
		qsort(c.acc, c.n_acc, sizeof *c.acc, by_merge);
	}
	size_t units = (bytes + chars) / sizeof(shclw_merge) + 1;
	// Not a ?:, which hides from CodeQL that the block is raw calloc memory
	// and not all shclw_merge (cpp/suspicious-pointer-scaling).
	shclw_merge *out = NULL;
	if (ok) {
		out = (shclw_merge *)calloc(units, sizeof(shclw_merge));
	}
	if (out) {
		size_t *lines = (size_t *)(out + c.n_acc);
		size_t nl = 0;
		for (size_t i = 0; i < c.n_acc; i++) {
			nl += c.acc[i].n_lines;
		}
		char *text = (char *)(lines + nl);
		for (size_t i = 0; i < c.n_acc; i++) {
			const merge_acc *m = &c.acc[i];
			memcpy(lines, m->lines, m->n_lines * sizeof(size_t));
			memcpy(text, m->path, m->path_len + 1);
			// As in shclw_i_copy_nodes, the analyzer loses the sums above.
			// NOLINTBEGIN(clang-analyzer-security.ArrayBound)
			out[i].path.p = text;
			out[i].path.n = m->path_len;
			out[i].lines = lines;
			out[i].n_lines = m->n_lines;
			out[i].layer = m->layer;
			// NOLINTEND(clang-analyzer-security.ArrayBound)
			lines += m->n_lines;
			text += m->path_len + 1;
		}
		*count = c.n_acc;
	}
	for (size_t i = 0; i < c.n_acc; i++) {
		free(c.acc[i].path);
		free(c.acc[i].lines);
	}
	free(c.acc);
	free(c.stack);
	return out;
}

shclw_kind shclw_i_check_mode(const char *path, unsigned mode, shclw_error *err) {
	if (mode & ~0777u) {
		char why[64];
		(void)snprintf(why, sizeof why, "mode %#o is more than permission bits", mode);
		return shclw_i_fail(err, SHCLW_ERR_BAD_VALUE, path, why, 0);
	}
	return shclw_i_ok(err);
}

static shclw_kind exists(const char *path, shclw_error *err) {
	return shclw_i_fail(err, SHCLW_ERR_EXISTS, path, "already there; nothing written", 0);
}

static shclw_kind create_failed(const char *path, int e, shclw_error *err) {
	return shclw_i_fail(err, e == ENOENT ? SHCLW_ERR_NOT_FOUND : SHCLW_ERR_IO, path, strerror(e), e);
}

// A name beside path for the new text: the folder, then at most 64 bytes of
// the base, cut where a character starts, as shcl cuts its temp names.
static char *temp_name(const char *path, int attempt) {
	const char *slash = strrchr(path, '/');
#ifdef _WIN32
	const char *back = strrchr(path, '\\');
	if (back && (!slash || back > slash)) {
		slash = back;
	}
#endif
	size_t dlen = slash ? (size_t)(slash - path) + 1 : 0;
	const char *base = path + dlen;
	size_t cut = strlen(base);
	if (cut > 64) {
		cut = 64;
		while (cut > 0 && ((unsigned char)base[cut] & 0xC0) == 0x80) {
			cut--;
		}
	}
	size_t n = dlen + cut + 64;
	char *tmp = (char *)malloc(n);
	if (!tmp) {
		return NULL;
	}
	// A local's address differs between threads racing in one process.
#ifdef _WIN32
	long pid = (long)GetCurrentProcessId();
#else
	long pid = (long)getpid();
#endif
	(void)snprintf(tmp, n, "%.*s.%.*s.new%ld.%lx.%d", (int)dlen, path, (int)cut, base, pid,
	               (unsigned long)(uintptr_t)&attempt, attempt);
	return tmp;
}

#ifdef _WIN32

// Writes data to a new file and syncs it. Returns 0 or an errno.
static int write_new_file(const char *path, const char *data, size_t len) {
	FILE *f = fopen(path, "wbx");
	if (!f) {
		return errno == EEXIST ? EEXIST : (errno ? errno : EIO);
	}
	int e = fwrite(data, 1, len, f) == len && fflush(f) == 0 && _commit(_fileno(f)) == 0 ? 0 : EIO;
	if (fclose(f) != 0 && !e) {
		e = EIO;
	}
	if (e) {
		(void)remove(path); // ours, made just above
	}
	return e;
}

// Windows moves a file into place only when nothing is there, without a hard
// link. There's no Unix mode to give it.
shclw_kind shclw_i_create_new(const char *path, const char *data, size_t len, unsigned mode, shclw_error *err) {
	shclw_kind k = shclw_i_check_mode(path, mode, err);
	if (k != SHCLW_OK) {
		return k;
	}
	char *tmp = NULL;
	int e = EEXIST;
	for (int attempt = 0; attempt < 8 && e == EEXIST; attempt++) {
		free(tmp);
		tmp = temp_name(path, attempt);
		if (!tmp) {
			return shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
		}
		e = write_new_file(tmp, data, len);
	}
	if (e) {
		free(tmp);
		return create_failed(path, e, err);
	}
	if (MoveFileExA(tmp, path, MOVEFILE_WRITE_THROUGH)) {
		free(tmp);
		return shclw_i_ok(err);
	}
	DWORD w = GetLastError();
	(void)remove(tmp);
	free(tmp);
	if (w == ERROR_ALREADY_EXISTS || w == ERROR_FILE_EXISTS) {
		return exists(path, err);
	}
	return create_failed(path, w == ERROR_PATH_NOT_FOUND ? ENOENT : EIO, err);
}

#else

// Writes data, sets mode, syncs, and closes fd. Returns 0 or an errno.
static int fill(int fd, const char *data, size_t len, unsigned mode) {
	int e = 0;
	while (len > 0) {
		ssize_t n = write(fd, data, len);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			e = errno;
			break;
		}
		data += n;
		len -= (size_t)n;
	}
	if (!e && mode && fchmod(fd, (mode_t)mode) != 0) {
		e = errno;
	}
	if (!e && fsync(fd) != 0) {
		e = errno;
	}
	if (close(fd) != 0 && !e) {
		e = errno;
	}
	return e;
}

// One with a mode is born private, and gets its mode exactly in fill, past
// the umask.
static int open_new(const char *path, unsigned mode) {
	return open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode ? 0600 : 0666);
}

static void sync_dir(const char *path) {
	const char *slash = strrchr(path, '/');
	char *dir = slash ? shclw_i_dup(path, (size_t)(slash - path) + 1, NULL) : NULL;
	int fd = open(dir ? dir : ".", O_RDONLY | O_CLOEXEC);
	free(dir);
	if (fd >= 0) {
		(void)fsync(fd); // some filesystems can't; the file itself is synced
		(void)close(fd);
	}
}

// shclw_i_create_new without a temp, for a filesystem with no hard links. A
// crash part way leaves part of the file.
static shclw_kind create_in_place(const char *path, const char *data, size_t len, unsigned mode, shclw_error *err) {
	int fd = open_new(path, mode);
	if (fd < 0) {
		return errno == EEXIST ? exists(path, err) : create_failed(path, errno, err);
	}
	int e = fill(fd, data, len, mode);
	if (e) {
		(void)unlink(path); // ours, made just above
		return create_failed(path, e, err);
	}
	sync_dir(path);
	return shclw_i_ok(err);
}

static bool no_links(int e) {
#if EOPNOTSUPP != ENOTSUP
	if (e == EOPNOTSUPP) {
		return true;
	}
#endif
	return e == EPERM || e == ENOTSUP || e == ENOSYS;
}

// The text is synced under a temp name and hard-linked into place, so the
// name never holds part of it, and of 2 creates at once exactly one wins.
shclw_kind shclw_i_create_new(const char *path, const char *data, size_t len, unsigned mode, shclw_error *err) {
	shclw_kind k = shclw_i_check_mode(path, mode, err);
	if (k != SHCLW_OK) {
		return k;
	}
	char *tmp = NULL;
	int fd = -1;
	for (int attempt = 0; attempt < 8 && fd < 0; attempt++) {
		free(tmp);
		tmp = temp_name(path, attempt);
		if (!tmp) {
			return shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
		}
		fd = open_new(tmp, mode);
		if (fd < 0 && errno != EEXIST) {
			break;
		}
	}
	if (fd < 0) {
		int e = errno;
		free(tmp);
		return create_failed(path, e, err);
	}
	int e = fill(fd, data, len, mode);
	if (!e && link(tmp, path) != 0) {
		e = errno;
		(void)unlink(tmp);
		free(tmp);
		if (e == EEXIST) {
			return exists(path, err);
		}
		return no_links(e) ? create_in_place(path, data, len, mode, err) : create_failed(path, e, err);
	}
	(void)unlink(tmp);
	free(tmp);
	if (e) {
		return create_failed(path, e, err);
	}
	sync_dir(path);
	return shclw_i_ok(err);
}

#endif

// Where uncomment's line comes from: the indent, then the body, then a '\r'
// when cr.
typedef struct {
	size_t indent;
	size_t body, body_n;
	bool cr;
} uncut_line;

static bool blank_at(const char *p, size_t i) { return p[i] == ' ' || p[i] == '\t'; }

// Where a trailing "## Default" note starts in l, or SIZE_MAX with none.
static size_t default_note(const char *l, size_t n) {
	static const char note[] = "default";
	size_t k = sizeof note - 1;
	size_t e = n;
	while (e > 0 && (blank_at(l, e - 1) || l[e - 1] == '\r')) {
		e--;
	}
	if (e < k) {
		return SIZE_MAX;
	}
	for (size_t i = 0; i < k; i++) {
		if (ascii_lower((unsigned char)l[e - k + i]) != (unsigned char)note[i]) {
			return SIZE_MAX;
		}
	}
	e -= k;
	while (e > 0 && blank_at(l, e - 1)) {
		e--;
	}
	if (e < 3 || l[e - 2] != '#' || l[e - 1] != '#' || !blank_at(l, e - 3)) {
		return SIZE_MAX;
	}
	return e - 2;
}

// The line at l, n bytes long, without its #, for a line with one # and then
// text with a colon in it. A ## line is a note, not a setting.
static bool uncomment(const char *l, size_t n, bool drop_note, uncut_line *u) {
	size_t i = 0;
	while (i < n && blank_at(l, i)) {
		i++;
	}
	if (i >= n || l[i] != '#' || (i + 1 < n && l[i + 1] == '#')) {
		return false;
	}
	size_t b = i + 1;
	while (b < n && blank_at(l, b)) {
		b++;
	}
	if (b >= n || l[b] == '#' || !memchr(l + b, ':', n - b)) {
		return false;
	}
	u->indent = i;
	u->body = b;
	u->body_n = n - b;
	u->cr = false;
	size_t at = drop_note ? default_note(l + b, n - b) : SIZE_MAX;
	if (at != SIZE_MAX) {
		u->cr = l[n - 1] == '\r';
		while (at > 0 && blank_at(l + b, at - 1)) {
			at--;
		}
		u->body_n = at;
	}
	return true;
}

// text with the line at start, n bytes long, as u has it. Caller frees.
static char *with_line(const char *text, size_t len, size_t start, size_t n, const uncut_line *u, size_t *out_len) {
	size_t mid = u->indent + u->body_n + (u->cr ? 1 : 0);
	size_t rest = len - start - n;
	size_t total = start + mid + rest;
	char *out = (char *)malloc(total + 1);
	if (!out) {
		return NULL;
	}
	memcpy(out, text, start);
	memcpy(out + start, text + start, u->indent);
	memcpy(out + start + u->indent, text + start + u->body, u->body_n);
	if (u->cr) {
		out[start + mid - 1] = '\r';
	}
	memcpy(out + start + mid, text + start + n, rest);
	out[total] = '\0';
	*out_len = total;
	return out;
}

typedef struct {
	char **paths;
	size_t n, cap;
} path_stack;

static bool push_path(path_stack *s, const char *p, size_t n) {
	if (s->n == s->cap) {
		size_t cap = s->cap ? s->cap * 2 : 16;
		char **grown =
			cap < SIZE_MAX / sizeof(char *) ? (char **)realloc((void *)s->paths, cap * sizeof(char *)) : NULL;
		if (!grown) {
			return false;
		}
		s->paths = grown;
		s->cap = cap;
	}
	char *dup = shclw_i_dup(p, n, NULL);
	if (!dup) {
		return false;
	}
	s->paths[s->n++] = dup;
	return true;
}

typedef bool (*field_fn)(void *ctx, shclw_doc *x, const char *parent, shclw_string name, const shclw_node *n);

// Every node in x, with the path it's under and its name, through see. False
// when memory ran out or see said stop.
static bool walk_fields(shclw_doc *x, field_fn see, void *ctx) {
	path_stack s = {NULL, 0, 0};
	bool ok = push_path(&s, "", 0);
	while (ok && s.n > 0) {
		char *path = s.paths[--s.n];
		size_t k = 0;
		shclw_string *names = shclw_names(x, path, &k);
		ok = names != NULL;
		for (size_t i = 0; ok && i < k; i++) {
			char *child = shclw_child(x, path, names[i].p, names[i].n, NULL);
			size_t m = 0;
			shclw_node *nodes = child ? shclw_each(x, child, &m) : NULL;
			ok = nodes != NULL;
			for (size_t j = 0; ok && j < m; j++) {
				ok = see(ctx, x, path, names[i], &nodes[j]) && push_path(&s, nodes[j].path.p, nodes[j].path.n);
			}
			free(nodes);
			free(child);
		}
		free(names);
		free(path);
	}
	while (s.n > 0) {
		free(s.paths[--s.n]);
	}
	free((void *)s.paths);
	return ok;
}

// Every field's name and line, less the ones on line skip.
typedef struct {
	shclw_string *names;
	size_t *lines;
	size_t n, cap;
	size_t skip;
} field_list;

static bool see_field(void *ctx, shclw_doc *x, const char *parent, shclw_string name, const shclw_node *n) {
	(void)x;
	(void)parent;
	field_list *l = (field_list *)ctx;
	if (n->line == l->skip) {
		return true;
	}
	if (l->n == l->cap) {
		size_t cap = l->cap ? l->cap * 2 : 32;
		if (cap >= SIZE_MAX / sizeof(shclw_string)) {
			return false;
		}
		shclw_string *names = (shclw_string *)realloc(l->names, cap * sizeof(shclw_string));
		if (!names) {
			return false;
		}
		l->names = names;
		size_t *lines = (size_t *)realloc(l->lines, cap * sizeof(size_t));
		if (!lines) {
			return false;
		}
		l->lines = lines;
		l->cap = cap;
	}
	char *dup = shclw_i_dup(name.p, name.n, NULL);
	if (!dup) {
		return false;
	}
	for (size_t i = 0; i < name.n; i++) {
		dup[i] = (char)ascii_lower((unsigned char)dup[i]);
	}
	l->names[l->n].p = dup;
	l->names[l->n].n = name.n;
	l->lines[l->n++] = n->line;
	return true;
}

static void free_fields(field_list *l) {
	for (size_t i = 0; i < l->n; i++) {
		free((void *)l->names[i].p);
	}
	free(l->names);
	free(l->lines);
}

static bool same_fields(const field_list *a, const field_list *b) {
	if (a->n != b->n) {
		return false;
	}
	for (size_t i = 0; i < a->n; i++) {
		if (a->lines[i] != b->lines[i] || a->names[i].n != b->names[i].n ||
		    memcmp(a->names[i].p, b->names[i].p, a->names[i].n) != 0) {
			return false;
		}
	}
	return true;
}

// The one field on line with nothing under it, with its path as a program
// would write it: no index on the last step, which a key already set
// elsewhere in the block gets once the line comes back.
typedef struct {
	size_t line;
	size_t found;
	char *path;
	size_t path_n;
	char *value;
	size_t value_n;
} leaf_find;

static bool see_leaf(void *ctx, shclw_doc *x, const char *parent, shclw_string name, const shclw_node *n) {
	leaf_find *f = (leaf_find *)ctx;
	if (n->line != f->line) {
		return true;
	}
	size_t k = 0;
	shclw_string *kids = shclw_names(x, n->path.p, &k);
	if (!kids) {
		return false;
	}
	free(kids);
	if (k > 0 || ++f->found > 1) {
		return true;
	}
	f->path = shclw_child(x, parent, name.p, name.n, &f->path_n);
	f->value = shclw_i_dup(n->value.p, n->value.n, &f->value_n);
	return f->path && f->value;
}

typedef struct {
	shclw_commented_line *items;
	size_t n, cap;
} commented_acc;

static void free_commented_acc(commented_acc *a) {
	for (size_t i = 0; i < a->n; i++) {
		free((void *)a->items[i].path.p);
		free((void *)a->items[i].value.p);
	}
	free(a->items);
}

// f's leaf as a found line, which takes its strings.
static bool add_commented(commented_acc *a, leaf_find *f, bool is_default) {
	if (a->n == a->cap) {
		size_t cap = a->cap ? a->cap * 2 : 8;
		shclw_commented_line *grown =
			cap < SIZE_MAX / sizeof(shclw_commented_line)
				? (shclw_commented_line *)realloc(a->items, cap * sizeof(shclw_commented_line))
				: NULL;
		if (!grown) {
			return false;
		}
		a->items = grown;
		a->cap = cap;
	}
	shclw_commented_line *c = &a->items[a->n++];
	c->path.p = f->path;
	c->path.n = f->path_n;
	c->value.p = f->value;
	c->value.n = f->value_n;
	c->line = f->line;
	c->is_default = is_default;
	f->path = NULL;
	f->value = NULL;
	return true;
}

// The found lines in one block, each string with a NUL after it.
static shclw_commented_line *pack_commented(const commented_acc *a, size_t *count) {
	*count = 0;
	size_t n = a->n;
	if (n > (SIZE_MAX / 2) / sizeof(shclw_commented_line)) {
		return NULL;
	}
	size_t bytes = n * sizeof(shclw_commented_line);
	for (size_t i = 0; i < n; i++) {
		size_t more = a->items[i].path.n + a->items[i].value.n + 2;
		if (a->items[i].path.n >= SIZE_MAX / 8 || a->items[i].value.n >= SIZE_MAX / 8 || more >= SIZE_MAX / 2 - bytes) {
			return NULL;
		}
		bytes += more;
	}
	shclw_commented_line *out = (shclw_commented_line *)shclw_i_alloc(bytes);
	if (!out) {
		return NULL;
	}
	char *text = (char *)(out + n);
	for (size_t i = 0; i < n; i++) {
		out[i] = a->items[i];
		const shclw_string *from[2] = {&a->items[i].path, &a->items[i].value};
		shclw_string *to[2] = {&out[i].path, &out[i].value};
		for (size_t k = 0; k < 2; k++) {
			if (from[k]->n) {
				memcpy(text, from[k]->p, from[k]->n);
			}
			// As in shclw_i_copy_nodes, the analyzer loses the sum above.
			text[from[k]->n] = '\0'; // NOLINT(clang-analyzer-security.ArrayBound)
			to[k]->p = text;
			to[k]->n = from[k]->n;
			text += from[k]->n + 1;
		}
	}
	*count = n;
	return out;
}

// Line at, n bytes long and numbered line, as a commented setting, added to a
// when uncommenting it alone adds no error and moves no other field.
static bool try_commented(const char *text, size_t len, size_t at, size_t n, size_t line, const field_list *base,
                          size_t errs, commented_acc *a) {
	uncut_line u;
	if (!uncomment(text + at, n, false, &u)) {
		return true;
	}
	size_t probe_len = 0;
	char *probe = with_line(text, len, at, n, &u, &probe_len);
	shclw_doc *p = probe ? shclw_parse(probe, probe_len) : NULL;
	free(probe);
	if (!p) {
		return false;
	}
	bool ok = true;
	if (shclw_errors(p) == errs) {
		field_list got = {NULL, NULL, 0, 0, line};
		ok = walk_fields(p, see_field, &got);
		if (ok && same_fields(base, &got)) {
			leaf_find f = {line, 0, NULL, 0, NULL, 0};
			ok = walk_fields(p, see_leaf, &f);
			if (ok && f.found == 1) {
				ok = add_commented(a, &f, default_note(text + at, n) != SIZE_MAX);
			}
			free(f.path);
			free(f.value);
		}
		free_fields(&got);
	}
	shclw_free(p);
	return ok;
}

bool shclw_i_same_lower(const char *a, const char *b, size_t n) {
	for (size_t i = 0; i < n; i++) {
		if (ascii_lower((unsigned char)a[i]) != ascii_lower((unsigned char)b[i])) {
			return false;
		}
	}
	return true;
}

// A quick test, before the parse that says for sure, of whether path ends
// with the key the n bytes of body start with.
static bool key_ends(const char *path, const char *body, size_t n) {
	const char *colon = (const char *)memchr(body, ':', n);
	size_t end = colon ? (size_t)(colon - body) : n;
	if (body[0] == '"' || body[0] == '\'') {
		const char *q = (const char *)memchr(body + 1, body[0], n - 1);
		const char *k = q ? (const char *)memchr(q + 1, ':', n - (size_t)(q + 1 - body)) : NULL;
		end = k ? (size_t)(k - body) : end;
	}
	while (end > 0 && blank_at(body, end - 1)) {
		end--;
	}
	size_t plen = strlen(path);
	if (plen == end) {
		return shclw_i_same_lower(path, body, end);
	}
	return plen > end && path[plen - end - 1] == '.' && shclw_i_same_lower(path + plen - end, body, end);
}

// End of the line starting at at: its '\n', or len. memchr never gets an
// empty range, since the analyzer has it find a '\n' there anyway.
static size_t line_end(const char *text, size_t len, size_t at) {
	const char *nl = at < len ? (const char *)memchr(text + at, '\n', len - at) : NULL;
	return nl ? at + (size_t)(nl - (text + at)) : len;
}

shclw_commented_line *shclw_i_commented_of(const char *text, size_t len, const char *path, size_t *count) {
	*count = 0;
	if (!text) {
		text = "";
		len = 0;
	}
	commented_acc a = {NULL, 0, 0};
	field_list base = {NULL, NULL, 0, 0, 0};
	shclw_doc *d = NULL;
	size_t errs = 0;
	bool ok = true;
	size_t line = 0;
	for (size_t at = 0; ok;) {
		size_t end = line_end(text, len, at);
		line++;
		uncut_line u;
		bool cand = uncomment(text + at, end - at, false, &u);
		if (cand && path) {
			cand = default_note(text + at, end - at) != SIZE_MAX && key_ends(path, text + at + u.body, u.body_n);
		}
		if (cand) {
			if (!d) {
				d = shclw_parse(text, len);
				ok = d && walk_fields(d, see_field, &base);
				errs = ok ? shclw_errors(d) : 0;
			}
			ok = ok && try_commented(text, len, at, end - at, line, &base, errs, &a);
		}
		if (end >= len) {
			break;
		}
		at = end + 1;
	}
	shclw_commented_line *out = ok ? pack_commented(&a, count) : NULL;
	free_commented_acc(&a);
	free_fields(&base);
	shclw_free(d);
	return out;
}

char *shclw_i_uncommented(const char *text, size_t len, size_t n, size_t *out_len) {
	*out_len = 0;
	size_t at = 0;
	for (size_t line = 1; line < n; line++) {
		size_t end = line_end(text, len, at);
		if (end >= len) {
			return shclw_i_dup(text, len, out_len);
		}
		at = end + 1;
	}
	size_t end = line_end(text, len, at);
	uncut_line u;
	if (!uncomment(text + at, end - at, true, &u)) {
		return shclw_i_dup(text, len, out_len);
	}
	return with_line(text, len, at, end - at, &u, out_len);
}
