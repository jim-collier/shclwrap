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
	case SHCLW_ERR_NO_MEMORY: return "nomemory";
	}
	return "unknown";
}

const char *shclw_severity_name(shclw_severity s) { return s == SHCLW_SEV_ERROR ? "error" : "hint"; }

shclw_kind shclw_i_fail(shclw_error *err, shclw_kind kind, const char *path, const char *why, int sys_errno) {
	if (!err) {
		return kind;
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
	return kind;
}

shclw_kind shclw_i_ok(shclw_error *err) {
	if (err) {
		err->kind = SHCLW_OK;
		err->sys_errno = 0;
		err->msg[0] = '\0';
	}
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
	shclw_kind k = shclw_i_set(d, path, v, err);
	return k == SHCLW_OK ? shclw_i_verify_put(d, path, v, err) : k;
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

// Nothing at path, not even a dangling link.
static bool missing(const char *path) {
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
		return shclw_i_fail(err, SHCLW_ERR_IO, path, changed, 0);
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
		return shclw_i_fail(err, SHCLW_ERR_IO, path, changed, 0);
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
	if (missing(path) && rename(name, path) == 0) {
		note(err, "the original was put back", NULL);
	} else {
		note(err, "the original is ", name);
	}
	free(name);
	return k;
}

shclw_upgraded *shclw_i_upgraded(const char *text, size_t len, int64_t from, bool changed, size_t unsure,
                                 const char *backup, const shclw_diag *diags, size_t n_diags) {
	size_t blen = backup ? strlen(backup) : 0;
	if (len >= SIZE_MAX / 4 || blen >= SIZE_MAX / 4 || n_diags > (SIZE_MAX / 4) / sizeof(shclw_diag)) {
		return NULL;
	}
	// The diagnostics go through their own block first, since its layout is
	// shclw_i_copy_diags' to keep.
	size_t n = 0;
	shclw_diag *ds = shclw_i_copy_diags(diags, n_diags, &n);
	if (!ds) {
		return NULL;
	}
	size_t head = sizeof(shclw_upgraded) + n * sizeof(shclw_diag);
	size_t strings = 0;
	for (size_t i = 0; i < n; i++) {
		strings += ds[i].code.n + ds[i].message.n + 2;
	}
	shclw_upgraded *u = strings < SIZE_MAX / 4 ? (shclw_upgraded *)malloc(head + strings + len + blen + 2) : NULL;
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
	return u;
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
