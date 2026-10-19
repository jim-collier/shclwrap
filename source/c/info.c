// Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
// SPDX-License-Identifier: MIT

// The info block and the Format line, as a save writes them after InfoBlock
// and StampFormat (design.md, Format and upgrade). A port of Go's
// shclw/info.go, and every version's adapter shares it.

#include "shclw_adapter.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char format_word[] = "##    Format";
static const char migrated_line[] = "##    Migrated from SHCL 2.x.";
static const char schema_head[] = "##    Schema   ";
static const char info_title[] = " This config file format is SHCL.";
static const char info_name[] = " \"Simple Hierarchical Config Language\"";
// Stands in for 2.x's block while shcl upgrades the text, so the new block
// can go where the old one was.
static const char info_mark[] = "# shclw: the info block goes here";
static const char bom[] = "\xEF\xBB\xBF";

typedef struct {
	const char *p;
	size_t n;
} view;

// One line and its own line end, so a CRLF file keeps them. The text points
// into the caller's text, the block, or a buffer that outlives the lines.
typedef struct {
	view text;
	const char *end;
} line;

// An added line ends the way most lines in the file do, LF on a tie, as
// shcl's stamp does.
typedef struct {
	line *v;
	size_t n, cap;
	const char *eol;
	bool oom;
} lines;

static size_t zlen(const char *z) { return strlen(z); }

static bool is(view v, const char *a, const char *b) {
	size_t an = zlen(a), bn = zlen(b);
	return v.n == an + bn && !memcmp(v.p, a, an) && !memcmp(v.p + an, b, bn);
}

static bool starts(view v, const char *a, const char *b) {
	size_t an = zlen(a), bn = zlen(b);
	return v.n >= an + bn && !memcmp(v.p, a, an) && !memcmp(v.p + an, b, bn);
}

// Without its indent or trailing blanks. Only ASCII ones, so every language
// agrees.
static view trim(view v) {
	while (v.n && (v.p[0] == ' ' || v.p[0] == '\t' || v.p[0] == '\r')) {
		v.p++;
		v.n--;
	}
	while (v.n && (v.p[v.n - 1] == ' ' || v.p[v.n - 1] == '\t' || v.p[v.n - 1] == '\r')) {
		v.n--;
	}
	return v;
}

static view trimmed(const lines *f, size_t i) {
	if (!f->v || i >= f->n) {
		view empty = {"", 0};
		return empty;
	}
	return trim(f->v[i].text);
}
static bool blank(const line *l) { return trim(l->text).n == 0; }

static void push(lines *f, view text, const char *end) {
	if (f->oom) {
		return;
	}
	if (f->n == f->cap) {
		size_t cap = f->cap ? f->cap * 2 : 32;
		line *grown = cap < SIZE_MAX / sizeof(line) ? (line *)realloc(f->v, cap * sizeof(line)) : NULL;
		if (!grown) {
			f->oom = true;
			return;
		}
		f->v = grown;
		f->cap = cap;
	}
	f->v[f->n].text = text;
	f->v[f->n].end = end;
	f->n++;
}

static void split(lines *f, const char *text, size_t len) {
	size_t crlf = 0;
	f->eol = "\n";
	for (size_t at = 0; at < len;) {
		const char *nl = (const char *)memchr(text + at, '\n', len - at);
		view l = {text + at, nl ? (size_t)(nl - (text + at)) : len - at};
		const char *end = "";
		if (nl) {
			end = "\n";
			if (l.n && l.p[l.n - 1] == '\r') {
				l.n--;
				end = "\r\n";
				crlf++;
			}
		}
		push(f, l, end);
		at += l.n + zlen(end);
	}
	if (crlf * 2 > f->n) {
		f->eol = "\r\n";
	}
}

static void add(lines *f, view text) {
	if (f->n && !*f->v[f->n - 1].end) {
		f->v[f->n - 1].end = f->eol;
	}
	push(f, text, f->eol);
}

// The lines with the dropped ones left out, malloc'd. drop may be NULL.
static char *join(const lines *f, const bool *drop, size_t *len) {
	size_t n = 0;
	for (size_t i = 0; i < f->n; i++) {
		if (!drop || !drop[i]) {
			n += f->v[i].text.n + zlen(f->v[i].end);
		}
	}
	char *out = (char *)malloc(n + 1);
	if (!out) {
		return NULL;
	}
	size_t at = 0;
	for (size_t i = 0; i < f->n; i++) {
		if (drop && drop[i]) {
			continue;
		}
		if (f->v[i].text.n) {
			memcpy(out + at, f->v[i].text.p, f->v[i].text.n);
		}
		at += f->v[i].text.n;
		size_t en = zlen(f->v[i].end);
		memcpy(out + at, f->v[i].end, en);
		at += en;
	}
	out[at] = '\0';
	*len = at;
	return out;
}

// Takes out the dropped lines. Where a run went, the the blank lines around it
// become one, or none at the top or the end of the file, so what was set
// apart stays apart.
static void remove_lines(lines *f, const bool *drop) {
	size_t w = 0;
	for (size_t i = 0; i < f->n;) {
		if (!drop[i] && !blank(&f->v[i])) {
			f->v[w++] = f->v[i++];
			continue;
		}
		// A stretch of blank and dropped lines.
		size_t k = i;
		bool gone = false;
		size_t first_blank = SIZE_MAX;
		for (; k < f->n && (drop[k] || blank(&f->v[k])); k++) {
			gone = gone || drop[k];
			if (!drop[k] && first_blank == SIZE_MAX) {
				first_blank = k;
			}
		}
		if (!gone) {
			// w <= i, so these move down or stay.
			for (size_t j = i; j < k; j++) {
				f->v[w++] = f->v[j];
			}
		} else if (first_blank != SIZE_MAX && w > 0 && k < f->n) {
			f->v[w++] = f->v[first_blank];
		}
		i = k;
	}
	f->n = w;
}

static void trim_blank_tail(lines *f) {
	while (f->n && blank(&f->v[f->n - 1])) {
		f->n--;
	}
}

// Whether text loads to the same values and errors as the base, through the
// adapter's shcl.
typedef struct {
	const shclw_i_info_edit *e;
	char *base;
	size_t base_n;
	bool oom;
} judge;

static bool same_as(judge *j, const char *text, size_t len, const char *other, size_t other_n) {
	if (j->oom) {
		return false;
	}
	size_t n = 0;
	char *v = j->e->values(text, len, &n);
	if (!v) {
		j->oom = true;
		return false;
	}
	bool same = n == other_n && (!n || !memcmp(v, other, n));
	free(v);
	return same;
}

// Whether the lines without the dropped ones load as the base does, so
// what's dropped is comments.
static bool comments(judge *j, const lines *f, const bool *drop) {
	size_t len = 0;
	char *text = j->oom ? NULL : join(f, drop, &len);
	if (!text) {
		j->oom = true;
		return false;
	}
	bool same = same_as(j, text, len, j->base, j->base_n);
	free(text);
	return same;
}

// A Format line read as shcl 3.0 reads it: the head, then digits only. ok is
// true for any line with the head, which comes off as a stamp, and the number
// is 0 when it doesn't read. Digits too big read as major, as shcl reads them.
static int64_t format_number(view t, int64_t major, bool *ok) {
	*ok = false;
	if (is(t, format_word, "")) {
		*ok = true;
		return 0;
	}
	if (!starts(t, format_word, "   ")) {
		return 0;
	}
	*ok = true;
	size_t head = sizeof format_word - 1 + 3;
	if (t.n == head) {
		return 0;
	}
	uint64_t v = 0;
	bool big = false;
	for (size_t i = head; i < t.n; i++) {
		if (t.p[i] < '0' || t.p[i] > '9') {
			return 0;
		}
		if (!big) {
			v = v * 10 + (uint64_t)(t.p[i] - '0');
			big = v > UINT32_MAX;
		}
	}
	return big ? major : (int64_t)v;
}

static bool *flags(size_t n) { return (bool *)calloc(n ? n : 1, sizeof(bool)); }

// Each info block and lone Format line, as [start, end) pairs in *runs, and
// how many. A block runs from the lone mark above its title line to the next
// lone mark, or, with none, through the lines written the block's way. That's
// shcl 3.0's rule for its own setter, with 2.x's `#` block found the same
// way. A Format line outside a block comes with the migrated note under it.
// NULL *runs with a count of SIZE_MAX means memory ran out.
static size_t info_runs(const lines *f, size_t **runs) {
	size_t count = 0;
	// No more runs than lines.
	size_t *r = (size_t *)malloc((f->n ? f->n : 1) * 2 * sizeof(size_t));
	*runs = r;
	if (!r) {
		return SIZE_MAX;
	}
	size_t n = f->n;
	for (size_t i = 0; i < n;) {
		view t = trimmed(f, i);
		const char *mark = is(t, "##", info_title) ? "##" : is(t, "#", info_title) ? "#" : NULL;
		if (!mark) {
			size_t end = i + 1;
			bool ok = false;
			(void)format_number(t, 0, &ok);
			if (ok) {
				if (end < n && is(trimmed(f, end), migrated_line, "")) {
					end++;
				}
				r[2 * count] = i;
				r[2 * count + 1] = end;
				count++;
			}
			i = end;
			continue;
		}
		size_t start = i, end = i + 1;
		if (i > 0 && is(trimmed(f, i - 1), mark, "") && (count == 0 || r[2 * count - 1] < i)) {
			start = i - 1;
		}
		bool closed = false;
		for (size_t k = end; k < n && starts(trimmed(f, k), mark, ""); k++) {
			if (is(trimmed(f, k), mark, "")) {
				end = k + 1;
				closed = true;
				break;
			}
		}
		while (!closed && end < n && (starts(trimmed(f, end), mark, "    ") || is(trimmed(f, end), mark, info_name))) {
			end++;
		}
		r[2 * count] = start;
		r[2 * count + 1] = end;
		count++;
		i = end;
	}
	return count;
}

// Each Format line that counts, in order: one whose removal leaves the
// values at the base, so one in a raw body doesn't. Stops at the first a
// check refuses, which is all the caller needs.
static shclw_kind check_named(judge *j, const lines *f, const char *path, bool *named, shclw_error *err) {
	bool *drop = flags(f->n);
	if (!drop) {
		return shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
	}
	shclw_kind kind = SHCLW_OK;
	for (size_t i = 0; i < f->n && kind == SHCLW_OK; i++) {
		bool ok = false;
		int64_t n = format_number(trimmed(f, i), j->e->major, &ok);
		if (!ok || n == 0) {
			continue;
		}
		drop[i] = true;
		bool counts = comments(j, f, drop);
		drop[i] = false;
		if (j->oom) {
			kind = shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
		} else if (counts) {
			char why[96];
			if (n > j->e->major) {
				(void)snprintf(why, sizeof why, "the file names format %" PRId64, n);
				kind = shclw_i_fail(err, SHCLW_ERR_TOO_NEW, path, why, 0);
			} else if (n < j->e->major) {
				(void)snprintf(why, sizeof why, "the file names format %" PRId64 "; upgrade it first", n);
				kind = shclw_i_fail(err, SHCLW_ERR_REFUSED, path, why, 0);
			}
			*named = true;
		}
	}
	free(drop);
	return kind;
}

// Takes off every old block and lone Format line that's comments, but for a
// lone stamp the save would add back. A Schema line in a block stays.
static shclw_kind drop_old(judge *j, lines *f, shclw_i_info info, bool stamp, bool *named, const char *path,
                           shclw_error *err) {
	size_t *runs = NULL;
	size_t count = info_runs(f, &runs);
	bool *drop = flags(f->n);
	bool *run = flags(f->n);
	if (!runs || !drop || !run) {
		free(runs);
		free(drop);
		free(run);
		return shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
	}
	for (size_t r = 0; r < count; r++) {
		size_t start = runs[2 * r], end = runs[2 * r + 1];
		bool lone = false;
		int64_t n = format_number(trimmed(f, start), j->e->major, &lone);
		if (lone && stamp && info == SHCLW_I_INFO_OFF && n == j->e->major) {
			*named = true;
			continue;
		}
		for (size_t i = start; i < end; i++) {
			// The analyzer loses that a run ends within the lines.
			run[i] = !starts(trimmed(f, i), schema_head, ""); // NOLINT(clang-analyzer-security.ArrayBound)
		}
		if (comments(j, f, run)) {
			for (size_t i = start; i < end; i++) {
				drop[i] = drop[i] || run[i];
			}
		}
		for (size_t i = start; i < end; i++) {
			run[i] = false;
		}
	}
	if (!j->oom) {
		remove_lines(f, drop);
	}
	free(runs);
	free(drop);
	free(run);
	return j->oom ? shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0) : SHCLW_OK;
}

static void add_block(lines *f, const char *block) {
	size_t len = zlen(block);
	if (len && block[len - 1] == '\n') {
		len--;
	}
	for (size_t at = 0; at <= len;) {
		const char *nl = (const char *)memchr(block + at, '\n', len - at);
		size_t n = nl ? (size_t)(nl - (block + at)) : len - at;
		view l = {block + at, n};
		add(f, l);
		at += n + 1;
	}
}

shclw_kind shclw_i_finish_info(const char *text, size_t len, shclw_i_info info, bool stamp, const shclw_i_info_edit *e,
                               const char *path, char **out, size_t *out_len, shclw_error *err) {
	*out = NULL;
	*out_len = 0;
	if (info == SHCLW_I_INFO_AS_IS && !stamp) {
		*out = shclw_i_dup(text, len, out_len);
		return *out ? shclw_i_ok(err) : shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
	}
	size_t bom_n = len >= 3 && !memcmp(text, bom, 3) ? 3 : 0;
	text += bom_n;
	len -= bom_n;

	lines f = {NULL, 0, 0, "\n", false};
	split(&f, text, len);
	judge j = {e, NULL, 0, f.oom};
	if (!j.oom) {
		j.base = e->values(text, len, &j.base_n);
		j.oom = !j.base;
	}
	char stamp_line[64];
	char *before = NULL, *after = NULL;
	size_t before_n = 0, after_n = 0;
	bool named = false;
	shclw_kind kind =
		j.oom ? shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0) : check_named(&j, &f, path, &named, err);
	if (kind == SHCLW_OK && info != SHCLW_I_INFO_AS_IS) {
		named = false;
		kind = drop_old(&j, &f, info, stamp, &named, path, err);
	}
	if (kind == SHCLW_OK) {
		before = join(&f, NULL, &before_n);
		if (info == SHCLW_I_INFO_ON) {
			trim_blank_tail(&f);
			if (f.n) {
				view nothing = {"", 0};
				add(&f, nothing);
			}
			add_block(&f, e->block);
			named = true;
		}
		if (stamp && !named) {
			int n = snprintf(stamp_line, sizeof stamp_line, "%s   %" PRId64, format_word, e->major);
			view l = {stamp_line, n > 0 ? (size_t)n : 0};
			add(&f, l);
		}
		after = f.oom ? NULL : join(&f, NULL, &after_n);
		if (!before || !after) {
			kind = shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
		}
	}
	if (kind == SHCLW_OK && before && after && (after_n != before_n || memcmp(after, before, after_n) != 0)) {
		// An added line that's the content of a raw block nothing closes.
		size_t vn = 0;
		char *v = e->values(before, before_n, &vn);
		j.oom = !v;
		bool same = v && same_as(&j, after, after_n, v, vn);
		free(v);
		if (j.oom) {
			kind = shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
		} else if (!same) {
			kind = shclw_i_fail(err, SHCLW_ERR_REFUSED, path, "the file ends in a raw block that never closes", 0);
		}
	}
	if (kind == SHCLW_OK && after) {
		char *done = (char *)malloc(bom_n + after_n + 1);
		if (done) {
			memcpy(done, bom, bom_n);
			memcpy(done + bom_n, after, after_n + 1);
			*out = done;
			*out_len = bom_n + after_n;
		} else {
			kind = shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
		}
	}
	free(before);
	free(after);
	free(j.base);
	free(f.v);
	return kind == SHCLW_OK ? shclw_i_ok(err) : kind;
}

static size_t bom_len(const char *text, size_t len) { return len >= 3 && !memcmp(text, bom, 3) ? 3 : 0; }

// head, then body, as one malloc'd string.
static char *glue(const char *head, size_t head_n, const char *body, size_t body_n, size_t *len) {
	char *out = (char *)malloc(head_n + body_n + 1);
	if (!out) {
		return NULL;
	}
	if (head_n) {
		memcpy(out, head, head_n);
	}
	if (body_n) {
		memcpy(out + head_n, body, body_n);
	}
	out[head_n + body_n] = '\0';
	*len = head_n + body_n;
	return out;
}

static size_t hits(const char *p, size_t n, const char *z) {
	size_t zn = zlen(z), count = 0;
	for (size_t i = 0; i + zn <= n; i++) {
		count += !memcmp(p + i, z, zn);
	}
	return count;
}

static bool old_title(const lines *f, size_t start, size_t end) {
	return is(trimmed(f, start), "#", info_title) || (start + 1 < end && is(trimmed(f, start + 1), "#", info_title));
}

bool shclw_i_old_block(const char *text, size_t len, const shclw_i_info_edit *e, bool *found, char **marked,
                       size_t *marked_len) {
	*found = false;
	*marked = NULL;
	*marked_len = 0;
	size_t bom_n = bom_len(text, len);
	if (!hits(text, len, info_title)) {
		return true;
	}
	lines f = {NULL, 0, 0, "\n", false};
	split(&f, text + bom_n, len - bom_n);
	judge j = {e, NULL, 0, f.oom};
	if (!j.oom) {
		j.base = e->values(text + bom_n, len - bom_n, &j.base_n);
		j.oom = !j.base;
	}
	size_t *runs = NULL;
	size_t count = j.oom ? 0 : info_runs(&f, &runs);
	bool *run = flags(f.n);
	j.oom = j.oom || !run || (count && !runs) || count == SIZE_MAX;
	for (size_t r = 0; !j.oom && !*found && r < count; r++) {
		size_t start = runs[2 * r], end = runs[2 * r + 1];
		if (!old_title(&f, start, end)) {
			continue;
		}
		size_t last = start;
		for (size_t i = start; i < end; i++) {
			// The analyzer loses that a run ends within the lines.
			run[i] = !starts(trimmed(&f, i), schema_head, ""); // NOLINT(clang-analyzer-security.ArrayBound)
			last = run[i] ? i : last;
		}
		if (!comments(&j, &f, run)) {
			memset(run, 0, f.n * sizeof *run);
			continue;
		}
		*found = true;
		bool taken = false;
		for (size_t i = 0; i < f.n; i++) {
			taken = taken || is(trimmed(&f, i), info_mark, "");
		}
		if (taken) {
			break;
		}
		view m = {info_mark, sizeof info_mark - 1};
		f.v[last].text = m;
		run[last] = false;
		size_t n = 0;
		char *joined = join(&f, run, &n);
		*marked = joined ? glue(text, bom_n, joined, n, marked_len) : NULL;
		j.oom = !*marked;
		free(joined);
	}
	free(runs);
	free(run);
	free(j.base);
	free(f.v);
	return !j.oom;
}

shclw_kind shclw_i_place_block(const char *text, size_t len, bool marked, const shclw_i_info_edit *e, const char *path,
                               char **out, size_t *out_len, shclw_error *err) {
	*out = NULL;
	*out_len = 0;
	char *off = NULL;
	size_t off_n = 0;
	shclw_kind k = shclw_i_finish_info(text, len, SHCLW_I_INFO_OFF, false, e, path, &off, &off_n, err);
	if (k != SHCLW_OK) {
		free(off);
		return k;
	}
	size_t bom_n = bom_len(off, off_n);
	const char *body = off + bom_n;
	size_t body_n = off_n - bom_n;
	lines f = {NULL, 0, 0, "\n", false};
	split(&f, body, body_n);
	bool *marks = flags(f.n);
	lines g = {NULL, 0, 0, f.eol, false};
	char *joined = NULL, *v = NULL;
	size_t n = 0, vn = 0, at = SIZE_MAX, count = 0;
	judge j = {e, NULL, 0, false};
	bool same = false;
	if (f.oom || !marks) {
		k = shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
		goto done;
	}
	for (size_t i = 0; i < f.n; i++) {
		if (is(trimmed(&f, i), info_mark, "")) {
			marks[i] = true;
			at = i;
			count++;
		}
	}
	if (!marked || count != 1 || hits(body, body_n, info_mark) != 1) {
		joined = marked && count ? join(&f, marks, &n) : NULL;
		char *bare = joined ? glue(off, bom_n, joined, n, &n) : NULL;
		if (marked && count && !bare) {
			k = shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
		} else {
			k = shclw_i_finish_info(bare ? bare : off, bare ? n : off_n, SHCLW_I_INFO_ON, false, e, path, out, out_len,
			                        err);
		}
		free(bare);
		goto done;
	}
	for (size_t i = 0; i < f.n; i++) {
		if (i != at) {
			push(&g, f.v[i].text, f.v[i].end);
			continue;
		}
		size_t blen = zlen(e->block);
		if (blen && e->block[blen - 1] == '\n') {
			blen--;
		}
		for (size_t b = 0; b <= blen;) {
			const char *nl = (const char *)memchr(e->block + b, '\n', blen - b);
			view l = {e->block + b, nl ? (size_t)(nl - (e->block + b)) : blen - b};
			push(&g, l, nl ? f.eol : f.v[i].end);
			b += l.n + 1;
		}
	}
	joined = g.oom ? NULL : join(&g, NULL, &n);
	v = joined ? e->values(body, body_n, &vn) : NULL;
	if (!v) {
		k = shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
		goto done;
	}
	same = same_as(&j, joined, n, v, vn);
	if (!j.oom && !same) {
		k = shclw_i_fail(err, SHCLW_ERR_REFUSED, path, "the file ends in a raw block that never closes", 0);
		goto done;
	}
	*out = j.oom ? NULL : glue(off, bom_n, joined, n, out_len);
	if (!*out) {
		k = shclw_i_fail(err, SHCLW_ERR_NO_MEMORY, path, NULL, 0);
	}
done:
	free(v);
	free(joined);
	free(g.v);
	free(marks);
	free(f.v);
	free(off);
	return k == SHCLW_OK ? shclw_i_ok(err) : k;
}

char *shclw_i_same_ends(const char *orig, size_t orig_len, const char *text, size_t len, const shclw_i_info_edit *e,
                        size_t *out_len) {
	lines f = {NULL, 0, 0, "\n", false};
	split(&f, orig, orig_len);
	bool crlf = !strcmp(f.eol, "\r\n");
	bool oom = f.oom;
	free(f.v);
	if (oom) {
		return NULL;
	}
	if (!crlf || memchr(text, '\r', len)) {
		return shclw_i_dup(text, len, out_len);
	}
	size_t lf = hits(text, len, "\n");
	char *conv = len + lf < len ? NULL : (char *)malloc(len + lf + 1);
	if (!conv) {
		return NULL;
	}
	size_t at = 0;
	// The analyzer loses that conv has room for a CR before every LF.
	for (size_t i = 0; i < len; i++) {
		if (text[i] == '\n') {
			conv[at++] = '\r'; // NOLINT(clang-analyzer-security.ArrayBound)
		}
		conv[at++] = text[i]; // NOLINT(clang-analyzer-security.ArrayBound)
	}
	conv[at] = '\0'; // NOLINT(clang-analyzer-security.ArrayBound)
	size_t vn = 0, cn = 0;
	char *v = e->values(text, len, &vn);
	char *c = v ? e->values(conv, at, &cn) : NULL;
	if (!c) {
		free(v);
		free(conv);
		return NULL;
	}
	bool same = vn == cn && (!vn || !memcmp(v, c, vn));
	free(v);
	free(c);
	if (!same) {
		free(conv);
		return shclw_i_dup(text, len, out_len);
	}
	*out_len = at;
	return conv;
}
