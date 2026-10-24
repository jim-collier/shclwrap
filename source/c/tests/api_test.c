// Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
// SPDX-License-Identifier: MIT

// The C API's own tests: what the shared cases can't see, such as who frees
// what, strings holding a NUL, and memory over many reads. The Makefile
// builds it plain, sanitized, and as C++.

// 3.0's shcl.h asks for POSIX too.
#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L // NOLINT(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
#endif

// The adapter header too, for the read-back check, which no 3.0 put fails.
#include "shclw_adapter.h"

#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#endif

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#if defined(__SANITIZE_ADDRESS__)
#define UNDER_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define UNDER_ASAN 1
#endif
#endif

static int failures;

// 3.0 writes an array in brackets.
#define LIST(items) "[" items "]"

#define STR_(x) #x
#define STR(x) STR_(x)

#define CHECK(cond)                                                                                                    \
	do {                                                                                                               \
		if (!(cond)) {                                                                                                 \
			(void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);                                           \
			failures++;                                                                                                \
		}                                                                                                              \
	} while (0)

static char scratch[4096];

// Windows keeps no mode past read-only, and has no fork, so the races there
// run their other side in a thread, as Python's tests do.
#ifdef _WIN32
#define HAS_MODES 0
static int new_folder(const char *path) { return _mkdir(path); }
static mode_t swap_umask(mode_t mask) { return mask; }
#else
#define HAS_MODES 1
static int new_folder(const char *path) { return mkdir(path, 0755); }
static mode_t swap_umask(mode_t mask) { return umask(mask); }
#endif

static const char *in_scratch(const char *name) {
	static char path[4200];
	(void)snprintf(path, sizeof path, "%s/%s", scratch, name);
	return path;
}

static void put_file(const char *name, const char *text, size_t len) {
	FILE *f = fopen(in_scratch(name), "wb");
	CHECK(f != NULL);
	if (f) {
		CHECK(fwrite(text, 1, len, f) == len);
		CHECK(fclose(f) == 0);
	}
}

static shclw_doc *parse_z(const char *text) { return shclw_parse(text, strlen(text)); }

static void test_reads(void) {
	shclw_doc *d = parse_z(
		"name: My App\nport: 8080\nratio: 0.25\ndebug: on\ntags: " LIST("red, green") "\nports: " LIST("80, 443") "\n");
	CHECK(d != NULL);

	shclw_got_int port = shclw_get_int(d, "port");
	CHECK(port.status == SHCLW_GOOD && port.value == 8080 && port.line == 2);
	shclw_got_float ratio = shclw_get_float(d, "ratio");
	CHECK(ratio.status == SHCLW_GOOD && ratio.value == 0.25);
	shclw_got_bool debug = shclw_get_bool(d, "debug");
	CHECK(debug.status == SHCLW_GOOD && debug.value);

	shclw_got_str name = shclw_get_str(d, "name");
	CHECK(name.status == SHCLW_GOOD && name.len == 6 && !strcmp(name.value, "My App") && name.line == 1);
	free(name.value);

	shclw_got_strs tags = shclw_get_strs(d, "tags");
	CHECK(tags.status == SHCLW_GOOD && tags.n == 2);
	if (tags.n == 2) {
		CHECK(!strcmp(tags.value[0].p, "red") && tags.value[1].n == 5);
	}
	free(tags.value);

	shclw_got_ints ports = shclw_get_ints(d, "ports");
	CHECK(ports.status == SHCLW_GOOD && ports.n == 2 && ports.value[1] == 443);
	free(ports.value);

	shclw_got_str missing = shclw_get_str(d, "missing");
	CHECK(missing.status == SHCLW_NOT_FOUND && missing.value && missing.len == 0 && missing.line == 0);
	free(missing.value);

	shclw_got_strs none = shclw_get_strs(d, "missing");
	CHECK(none.status == SHCLW_NOT_FOUND && none.value != NULL && none.n == 0);
	free(none.value);

	CHECK(shclw_int(d, "name", 7) == 7);
	CHECK(shclw_bool(d, "nope", true));
	CHECK(!strcmp(shclw_status_name(SHCLW_BAD_PATH), "BadPath"));
	CHECK(!strcmp(shclw_kind_name(SHCLW_ERR_NO_READ_BACK), "noreadback"));
	shclw_free(d);
	shclw_free(NULL);
}

static void test_fallbacks(void) {
	shclw_doc *d = parse_z("big: 1e400\nbigs: " LIST("1, 1e400") "\n");
	shclw_got_float big = shclw_get_float(d, "big");
	CHECK(big.status == SHCLW_BAD_TYPE && big.value == 0);
	CHECK(shclw_float(d, "big", 1.5) == 1.5);
	shclw_got_floats bigs = shclw_get_floats(d, "bigs");
	CHECK(bigs.status == SHCLW_BAD_TYPE && bigs.n == 2 && bigs.value[0] == 1 && bigs.value[1] == 0);
	free(bigs.value);

	size_t len = 99;
	char *s = shclw_str(d, "missing", NULL, 0, &len);
	CHECK(s && len == 0 && s[0] == '\0');
	free(s);
	s = shclw_str(d, "missing", "a\0b", 3, &len);
	CHECK(s && len == 3 && s[1] == '\0' && s[2] == 'b' && s[3] == '\0');
	free(s);

	// The fallback comes back as a copy, so the caller's array can go.
	char word[] = "kept";
	shclw_string fall[2];
	fall[0].p = word;
	fall[0].n = 4;
	fall[1].p = "";
	fall[1].n = 0;
	size_t n = 0;
	shclw_string *v = shclw_strs(d, "missing", fall, 2, &n);
	word[0] = 'X';
	CHECK(v && n == 2 && !strcmp(v[0].p, "kept") && v[1].n == 0 && v[1].p[0] == '\0');
	free(v);

	double ffall[1] = {2.5};
	double *f = shclw_floats(d, "bigs", ffall, 1, &n);
	CHECK(f && n == 1 && f[0] == 2.5);
	free(f);
	int64_t *ints = shclw_ints(d, "missing", NULL, 0, &n);
	CHECK(ints != NULL && n == 0);
	free(ints);
	shclw_free(d);
}

// 2026101513330047: a value can hold a NUL, and the text and the save keep
// every byte after one.
static void test_nul(void) {
	static const char nuls[] = "k: \"a\xE2\x97\x89NUL\xE2\x97\x89z\"\nl: [x, \"y\xE2\x97\x89NUL\xE2\x97\x89\"]\n";
	shclw_doc *d = shclw_parse(nuls, sizeof nuls - 1);
	shclw_got_str k = shclw_get_str(d, "k");
	CHECK(k.status == SHCLW_GOOD && k.len == 3 && k.value[1] == '\0' && k.value[2] == 'z' && k.value[3] == '\0');
	free(k.value);
	shclw_got_strs l = shclw_get_strs(d, "l");
	CHECK(l.status == SHCLW_GOOD && l.n == 2 && l.value[1].n == 2 && l.value[1].p[1] == '\0');
	free(l.value);
	shclw_free(d);

	static const char text[] = "# a\0b\nk: 1\n";
	d = shclw_parse(text, sizeof text - 1);
	size_t len = 0;
	char *out = shclw_text(d, &len);
	CHECK(out && len == sizeof text - 1 && !memcmp(out, text, len));
	free(out);
	CHECK(shclw_write(d, in_scratch("nul0.shcl"), NULL) == SHCLW_OK);
	char *saved = NULL;
	CHECK(shclw_read_text(in_scratch("nul0.shcl"), &saved, &len, NULL) == SHCLW_OK);
	CHECK(saved && len == sizeof text - 1 && !memcmp(saved, text, len));
	free(saved);
	shclw_free(d);
}

// 2026101513330046: shcl keeps what each read hands out until the document is
// freed. The wrapper copies it out and gives it back, so reads don't grow.
static void test_reads_give_back(void) {
#if defined(__GLIBC__) && !defined(UNDER_ASAN)
	shclw_doc *d = parse_z("tags: [alpha, beta, gamma, delta, epsilon]\nname: \"some text here\"\n");
	// The first pass sets up the block shcl keeps for its biggest read.
	struct mallinfo2 before = mallinfo2();
	for (int i = 0; i < 200001; i++) {
		if (i == 1) {
			before = mallinfo2();
		}
		shclw_got_strs g = shclw_get_strs(d, "tags");
		free(g.value);
		char *s = shclw_str(d, "tags", NULL, 0, NULL);
		free(s);
	}
	struct mallinfo2 after = mallinfo2();
	size_t grew = after.uordblks > before.uordblks ? after.uordblks - before.uordblks : 0;
	if (grew > (size_t)64 * 1024) {
		(void)fprintf(stderr, "200,000 reads kept %zu bytes\n", grew);
	}
	CHECK(grew <= (size_t)64 * 1024);
	shclw_free(d);
#else
	(void)puts("api: memory over many reads isn't measured in this build");
#endif
}

static void test_errors(void) {
	shclw_error err;
	shclw_doc *d = NULL;
	CHECK(shclw_open(in_scratch("missing.shcl"), &d, &err) == SHCLW_ERR_NOT_FOUND);
	CHECK(err.kind == SHCLW_ERR_NOT_FOUND && strstr(err.msg, "missing.shcl") && err.sys_errno == ENOENT);
	CHECK(d != NULL && shclw_int(d, "port", 9) == 9);
	shclw_free(d);

	put_file("bad.shcl", "k: \xff\n", 4);
	CHECK(shclw_open(in_scratch("bad.shcl"), &d, NULL) == SHCLW_ERR_IO);
	shclw_free(d);

	const char newer[] = "k: 1\n##    Format   9\n";
	put_file("newer.shcl", newer, sizeof newer - 1);
	// A newer file opens read-only.
	CHECK(shclw_open(in_scratch("newer.shcl"), &d, &err) == SHCLW_OK && shclw_read_only(d));
	CHECK(shclw_int(d, "k", 0) == 1);
	CHECK(shclw_write(d, in_scratch("newer.shcl"), &err) == SHCLW_ERR_TOO_NEW);
	CHECK(strstr(err.msg, "format 9") != NULL);
	shclw_free(d);

	const char good[] = "k: 1\n";
	put_file("good.shcl", good, sizeof good - 1);
	CHECK(shclw_open(in_scratch("good.shcl"), &d, &err) == SHCLW_OK && err.kind == SHCLW_OK);
	CHECK(shclw_int(d, "k", 0) == 1);
	CHECK(shclw_write(d, in_scratch("no/such/dir/x.shcl"), &err) == SHCLW_ERR_NOT_FOUND);
	shclw_free(d);

	const char bad[] = "a: 1\n1x y: 2\n";
	CHECK(shclw_parse_with(bad, sizeof bad - 1, SHCLW_STRICT, &d, &err) == SHCLW_ERR_STRICT);
	CHECK(d != NULL && shclw_errors(d) == 1);
	size_t n = 0;
	shclw_diag *ds = shclw_diags(d, &n);
	CHECK(ds && n == 1 && ds[0].line == 2 && ds[0].severity == SHCLW_SEV_ERROR && ds[0].code.n == 4);
	if (ds && n == 1) {
		CHECK(!strcmp(ds[0].code.p, "E014") && ds[0].message.n == strlen(ds[0].message.p));
	}
	free(ds);
	shclw_free(d);
	put_file("strict.shcl", bad, sizeof bad - 1);
	CHECK(shclw_open_with(in_scratch("strict.shcl"), SHCLW_STRICT, &d, &err) == SHCLW_ERR_STRICT);
	CHECK(strstr(err.msg, "strict.shcl") != NULL);
	shclw_free(d);
	const char bad_stamped[] = "a: 1\n1x y: 2\n##    Format   3\n";
	put_file("strict.shcl", bad_stamped, sizeof bad_stamped - 1);
	CHECK(shclw_open_with(in_scratch("strict.shcl"), SHCLW_STRICT, &d, &err) == SHCLW_ERR_STRICT);
	CHECK(strstr(err.msg, "strict.shcl") != NULL);
	shclw_free(d);

	// A save that would drop a line the load couldn't use, once an edit made
	// the kept lines impossible.
	const char lossy[] = "a: 1\na(5): 2\n";
	d = shclw_parse(lossy, sizeof lossy - 1);
	CHECK(shcl_remove(shclw_inner(d), "a", 1) == 1);
	CHECK(shclw_write(d, in_scratch("lossy.shcl"), &err) == SHCLW_ERR_REFUSED);
	CHECK(access(in_scratch("lossy.shcl"), F_OK) != 0);
	shclw_free(d);

	d = shclw_new();
	ds = shclw_diags(d, &n);
	CHECK(ds != NULL && n == 0);
	free(ds);
	shclw_free(d);
}

static bool text_is(shclw_doc *d, const char *want) {
	size_t len = 0;
	char *t = shclw_text(d, &len);
	bool same = t && len == strlen(want) && !memcmp(t, want, len);
	if (t && !same) {
		(void)fprintf(stderr, "text: %s\n", t);
	}
	free(t);
	return same;
}

static void test_walk(void) {
	shclw_doc *d = parse_z("a: 1\nb:\n\tc: 2\n\tC: 3\n\t\"\xC3\x89\": 4\n\t\"\xC3\xA9\": 5\n\tc: 6\n");
	CHECK(shclw_has(d, "b.c") && !shclw_has(d, "nope"));
	CHECK(shclw_count(d, "b.c") == 3 && shclw_count(d, "nope") == 0);
	CHECK(shclw_line(d, "a") == 1 && shclw_line(d, "b.c") == 0);
	// ASCII case only, as shcl matches names. É and é are 2 keys.
	size_t n = 0;
	shclw_string *names = shclw_names(d, "b", &n);
	CHECK(names && n == 3);
	if (names && n == 3) {
		CHECK(!strcmp(names[0].p, "c") && names[1].n == 2 && names[2].n == 2 && names[1].p[1] != names[2].p[1]);
	}
	free(names);
	names = shclw_names(d, "nope", &n);
	CHECK(names && n == 0);
	free(names);

	shclw_node *nodes = shclw_each(d, "b.c", &n);
	CHECK(nodes && n == 3);
	for (size_t i = 0; nodes && i < n; i++) {
		CHECK(shclw_line(d, nodes[i].path.p) == nodes[i].line && nodes[i].path.n == strlen(nodes[i].path.p));
	}
	if (nodes && n == 3) {
		CHECK(shclw_int(d, nodes[1].path.p, 0) == 3 && !strcmp(nodes[2].value.p, "6"));
	}
	free(nodes);
	shclw_free(d);

	// A wildcard lists the nodes it reaches, each read alone.
	d = parse_z("a:\n\tx:\n\t\tb: 1\n\tz:\n\t\tb: 2\n\ty:\n\t\tc: 3\n\tx: again\n\t\tb: 4\n");
	nodes = shclw_each(d, "a.*.b", &n);
	CHECK(nodes && n == 3);
	for (size_t i = 0; nodes && n == 3 && i < n; i++) {
		static const size_t lines[] = {3, 5, 9};
		static const int64_t values[] = {1, 2, 4};
		CHECK(nodes[i].line == lines[i] && shclw_int(d, nodes[i].path.p, 0) == values[i]);
	}
	free(nodes);
	for (size_t pass = 0; pass < 16; pass++) {
		nodes = shclw_each(d, "a.*.b", &n);
		CHECK(nodes && n == 3);
		free(nodes);
		nodes = shclw_each(d, "missing.*.b", &n);
		CHECK(nodes && n == 0);
		free(nodes);
	}
	char *last = shclw_last(d, "a.*.b", NULL);
	CHECK(last && shclw_int(d, last, 0) == 4);
	free(last);
	shclw_free(d);

	// A name that needs quoting, and one holding a NUL, still read through
	// shclw_child.
	d = shclw_new();
	CHECK(shclw_put_int(d, "\"x.y\"", 1, NULL) == SHCLW_OK);
	size_t len = 0;
	char *child = shclw_child(d, "", "x.y", 3, &len);
	CHECK(child && len == strlen(child) && shclw_int(d, child, 0) == 1);
	free(child);
	child = shclw_child(d, "top", "a\0b", 3, &len);
	CHECK(child && len == strlen(child) && !strncmp(child, "top.", 4));
	CHECK(child && shclw_put_int(d, child, 7, NULL) == SHCLW_OK && shclw_int(d, child, 0) == 7);
	free(child);
	shclw_free(d);
}

// First and Last pick by line, and an instance a put made sorts last with
// line 0. Go's TestFirstLast.
static void test_first_last(void) {
	shclw_doc *d = parse_z("p: 1\ntv: a\n\tf: 1\ntv: b\n\tf: 2\ntv: a\n\tf: 3\np: 2\n");
	char *p = shclw_last(d, "p", NULL);
	CHECK(p && shclw_int(d, p, 9) == 2);
	CHECK(p && shclw_put_int(d, p, 5, NULL) == SHCLW_OK);
	free(p);
	p = shclw_first(d, "tv.f", NULL);
	CHECK(p && shclw_int(d, p, 9) == 1);
	free(p);
	p = shclw_last(d, "tv.f", NULL);
	CHECK(p && shclw_int(d, p, 9) == 3);
	free(p);
	CHECK(shclw_int(d, "p", 9) == 9);
	size_t n = 0;
	size_t *lines = shclw_lines(d, "p", &n);
	CHECK(lines && n == 2 && lines[0] == 1 && lines[1] == 8);
	free(lines);

	shclw_node *tv = shclw_each(d, "tv", &n);
	CHECK(tv && n == 2);
	if (tv && n == 2) {
		char *g1 = shclw_child(d, tv[1].path.p, "g", 1, NULL);
		char *g0 = shclw_child(d, tv[0].path.p, "g", 1, NULL);
		CHECK(g1 && g0 && shclw_put_int(d, g1, 2, NULL) == SHCLW_OK && shclw_put_int(d, g0, 1, NULL) == SHCLW_OK);
		free(g1);
		free(g0);
	}
	free(tv);
	p = shclw_last(d, "tv.g", NULL);
	CHECK(p && shclw_int(d, p, 9) == 2);
	free(p);
	lines = shclw_lines(d, "tv.g", &n);
	CHECK(lines && n == 2 && lines[0] == 0 && lines[1] == 0);
	free(lines);
	shclw_free(d);

	// A put-made instance counts after the ones from the file, though Each
	// lists it first.
	d = parse_z("tv: a\ntv: b\n\tf: 1\n");
	tv = shclw_each(d, "tv", &n);
	char *f0 = tv && n == 2 ? shclw_child(d, tv[0].path.p, "f", 1, NULL) : NULL;
	CHECK(f0 && shclw_put_int(d, f0, 2, NULL) == SHCLW_OK);
	free(f0);
	free(tv);
	p = shclw_first(d, "tv.f", NULL);
	CHECK(p && shclw_int(d, p, 9) == 1);
	free(p);
	p = shclw_last(d, "tv.f", NULL);
	CHECK(p && shclw_int(d, p, 9) == 2);
	free(p);
	lines = shclw_lines(d, "tv.f", &n);
	CHECK(lines && n == 2 && lines[0] == 3 && lines[1] == 0);
	free(lines);
	size_t len = 0;
	p = shclw_last(d, "nope", &len);
	CHECK(p && len == 4 && !strcmp(p, "nope"));
	free(p);
	lines = shclw_lines(d, "nope", &n);
	CHECK(lines && n == 0);
	free(lines);
	shclw_free(d);
}

static void test_puts(void) {
	shclw_doc *d = shclw_new();
	shclw_error err;
	CHECK(shclw_put_int(d, "i", -7, &err) == SHCLW_OK && err.kind == SHCLW_OK);
	CHECK(shclw_put_float(d, "f", 0.25, NULL) == SHCLW_OK);
	CHECK(shclw_put_bool(d, "b", true, NULL) == SHCLW_OK);
	CHECK(shclw_put_str(d, "s", "a\0b", 3, NULL) == SHCLW_OK);
	shclw_string items[2];
	items[0].p = "x\0y";
	items[0].n = 3;
	items[1].p = "";
	items[1].n = 0;
	CHECK(shclw_put_strs(d, "ss", items, 2, NULL) == SHCLW_OK);
	static const int64_t ints[] = {1, -2, INT64_MAX};
	CHECK(shclw_put_ints(d, "is", ints, 3, NULL) == SHCLW_OK);
	static const double floats[] = {1.5, -0.125};
	CHECK(shclw_put_floats(d, "fs", floats, 2, NULL) == SHCLW_OK);
	CHECK(shclw_put_ints(d, "none", NULL, 0, NULL) == SHCLW_OK);
	static const char lit[] = LIST("80, 443");
	CHECK(shclw_put_syntax(d, "lit", lit, sizeof lit - 1, NULL) == SHCLW_OK);
	CHECK(shclw_comment(d, "i", "the i", 5, NULL) == SHCLW_OK);

	CHECK(shclw_int(d, "i", 0) == -7 && shclw_float(d, "f", 0) == 0.25 && shclw_bool(d, "b", false));
	shclw_got_str s = shclw_get_str(d, "s");
	CHECK(s.status == SHCLW_GOOD && s.len == 3 && s.value[1] == '\0' && s.value[2] == 'b');
	free(s.value);
	shclw_got_strs ss = shclw_get_strs(d, "ss");
	CHECK(ss.status == SHCLW_GOOD && ss.n == 2 && ss.value[0].n == 3 && ss.value[1].n == 0);
	free(ss.value);
	shclw_got_ints li = shclw_get_ints(d, "lit");
	CHECK(li.status == SHCLW_GOOD && li.n == 2 && li.value[1] == 443);
	free(li.value);

	// Refused values change nothing.
	size_t before_len = 0;
	char *before = shclw_text(d, &before_len);
	CHECK(shclw_put_float(d, "f", NAN, &err) == SHCLW_ERR_BAD_VALUE && strstr(err.msg, "finite"));
	CHECK(shclw_put_float(d, "g", INFINITY, NULL) == SHCLW_ERR_BAD_VALUE);
	CHECK(shclw_put_float32(d, "g", -INFINITY, NULL) == SHCLW_ERR_BAD_VALUE);
	static const double bad[] = {1, NAN};
	CHECK(shclw_put_floats(d, "fs", bad, 2, NULL) == SHCLW_ERR_BAD_VALUE);
	CHECK(shclw_put_int(d, "a.*.b", 1, &err) == SHCLW_ERR_BAD_PATH && strstr(err.msg, "a.*.b"));
	CHECK(shclw_comment(d, "i", "two\nlines", 9, NULL) == SHCLW_ERR_BAD_VALUE);
	CHECK(shclw_put_syntax(d, "lit", "\"1,", 3, NULL) == SHCLW_ERR_BAD_VALUE);
	CHECK(!shclw_has(d, "g"));
	size_t after_len = 0;
	char *after = shclw_text(d, &after_len);
	CHECK(before && after && before_len == after_len && !memcmp(before, after, after_len));
	free(before);
	free(after);

	// PutSyntax's whole text has to read back. The edit stays.
	CHECK(shclw_put_syntax(d, "c", "a # b", 5, &err) == SHCLW_ERR_NO_READ_BACK && err.kind == SHCLW_ERR_NO_READ_BACK);
	CHECK(shclw_has(d, "c"));
	shclw_free(d);
}

// Go's TestVerifyPut.
static void test_verify_put(void) {
	shclw_doc *d = parse_z("a: 1\n");
	shclw_i_value v = shclw_i_value_of(SHCLW_I_INT, 0);
	v.i = 1;
	CHECK(shclw_i_verify_put(d, "a", &v, NULL) == SHCLW_OK);
	v.i = 2;
	shclw_error err;
	CHECK(shclw_i_verify_put(d, "a", &v, &err) == SHCLW_ERR_NO_READ_BACK && strstr(err.msg, "different value"));
	v = shclw_i_value_of(SHCLW_I_STRS, 0);
	CHECK(shclw_i_verify_put(d, "nope", &v, &err) == SHCLW_ERR_NO_READ_BACK && strstr(err.msg, "NotFound"));
	shclw_string item;
	item.p = "1";
	item.n = 1;
	v = shclw_i_value_of(SHCLW_I_STRS, 1);
	v.strs = &item;
	CHECK(shclw_i_verify_put(d, "a", &v, NULL) == SHCLW_OK);
	item.p = "1\0";
	item.n = 2;
	CHECK(shclw_i_verify_put(d, "a", &v, NULL) == SHCLW_ERR_NO_READ_BACK);
	shclw_free(d);
}

// A float32 goes in as its own shortest digits. Go's TestPutFloat32, with
// want from Go's strconv.FormatFloat(float64(f), 'g', -1, 32).
static void test_put_float32(void) {
	static const struct {
		float f;
		double want;
		const char *text;
	} cases[] = {
		{0.9f, 0.9, "f: 0.9\n"},
		{0.1f, 0.1, "f: 0.1\n"},
		{-2.5f, -2.5, "f: -2.5\n"},
		{1e-10f, 1e-10, NULL},
		{FLT_MAX, 3.4028235e+38, NULL},
		{16777216, 1.6777216e+07, NULL},
		{FLT_TRUE_MIN, 1e-45, NULL},
		// 2^-12 is an exact tie at 8 digits. Go rounds it up, where glibc's
	    // printf goes to even.
		{0x1p-12f, 0.00024414063, NULL},
		{-0.0f, -0.0, NULL},
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		shclw_doc *d = shclw_new();
		CHECK(shclw_put_float32(d, "f", cases[i].f, NULL) == SHCLW_OK);
		shclw_got_float g = shclw_get_float(d, "f");
		CHECK(g.status == SHCLW_GOOD && g.value == cases[i].want && (float)g.value == cases[i].f);
		CHECK(signbit(g.value) == signbit(cases[i].want));
		if (cases[i].text) {
			CHECK(text_is(d, cases[i].text));
		}
		shclw_free(d);
	}
	shclw_doc *d = shclw_new();
	CHECK(shclw_put_float32(d, "f", NAN, NULL) == SHCLW_ERR_BAD_VALUE && !shclw_has(d, "f"));
	shclw_free(d);
}

// The digits floats print and float32 puts write with, against Go's
// strconv.FormatFloat. An exact tie goes to even. At a power of 2 it goes
// up, except the one Go leaves at even.
static void test_shortest(void) {
	static const struct {
		double f;
		const char *digits;
		int exp;
		bool single;
	} cases[] = {
		{0x1p-25, "29802322387695312", -8, false},
		{-0x1p-25, "29802322387695312", -8, false},
		{0x1p-12, "24414063", -4, true},
		{0x1p-54, "5551115", -17, true},
		{7.120236347223045e-307, "7120236347223045", -307, false},
		{0.1, "1", -1, false},
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		char digits[40];
		int exp = 0;
		shclw_i_shortest(cases[i].f, cases[i].single, digits, &exp);
		size_t n = strlen(digits);
		while (n > 1 && digits[n - 1] == '0') {
			digits[--n] = '\0';
		}
		if (strcmp(digits, cases[i].digits) != 0 || exp != cases[i].exp) {
			(void)fprintf(stderr, "shortest %a: %s e%d\n", cases[i].f, digits, exp);
		}
		CHECK(!strcmp(digits, cases[i].digits) && exp == cases[i].exp);
	}
}

// tv and u.v repeat at an inner step, n at the last one. Go's
// TestEditsRefuseSeveral, TestEditsOnOneInstance, and TestRemoveTakesAll.
static const char several[] = "tv: one\n\tg: 1\ntv: two\nn: 1\nn: 2\nu:\n\tv: 1\n\tv: 2\n\t\tw: 1\n";

// How many lines of a and of b sit between their common first and last
// lines. Go's hunk.
static bool hunk(const char *a, const char *b, size_t *gone, size_t *added) {
	enum { MAX_LINES = 64 };
	const char *la[MAX_LINES];
	const char *lb[MAX_LINES];
	size_t na = 0;
	size_t nb = 0;
	for (const char *p = a; *p && na < MAX_LINES; p = strchr(p, '\n') + 1) {
		la[na++] = p;
		if (!strchr(p, '\n')) {
			return false;
		}
	}
	for (const char *p = b; *p && nb < MAX_LINES; p = strchr(p, '\n') + 1) {
		lb[nb++] = p;
		if (!strchr(p, '\n')) {
			return false;
		}
	}
	size_t head = 0;
	while (head < na && head < nb && strcspn(la[head], "\n") == strcspn(lb[head], "\n") &&
	       !strncmp(la[head], lb[head], strcspn(la[head], "\n"))) {
		head++;
	}
	size_t tail = 0;
	while (tail < na - head && tail < nb - head) {
		const char *x = la[na - 1 - tail];
		const char *y = lb[nb - 1 - tail];
		if (strcspn(x, "\n") != strcspn(y, "\n") || strncmp(x, y, strcspn(x, "\n")) != 0) {
			break;
		}
		tail++;
	}
	*gone = na - head - tail;
	*added = nb - head - tail;
	return true;
}

static shclw_kind edit(shclw_doc *d, int which, const char *path) {
	switch (which) {
	case 0: return shclw_put_int(d, path, 5, NULL);
	case 1: return shclw_put_syntax(d, path, "6", 1, NULL);
	default: return shclw_comment(d, path, "c", 1, NULL);
	}
}

static void test_several(void) {
	static const char *const paths[] = {"n", "n.x", "tv", "tv.g", "tv.h", "tv.g.h", "u.v", "u.v.w", "u.v.w.x"};
	for (int which = 0; which < 3; which++) {
		for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
			shclw_doc *d = parse_z(several);
			CHECK(edit(d, which, paths[i]) == SHCLW_ERR_BAD_PATH);
			CHECK(text_is(d, several));
			shclw_free(d);
		}
	}

	// Paths from Each, First, and Last name one instance at every step.
	shclw_doc *d = parse_z(several);
	char *one[8] = {NULL};
	size_t at = 0;
	size_t n = 0;
	shclw_node *nodes = shclw_each(d, "tv", &n);
	for (size_t i = 0; nodes && i < n && at < 8; i++) {
		one[at++] = shclw_child(d, nodes[i].path.p, "h", 1, NULL);
	}
	free(nodes);
	nodes = shclw_each(d, "n", &n);
	for (size_t i = 0; nodes && i < n && at < 8; i++) {
		one[at] = (char *)malloc(nodes[i].path.n + 1);
		if (one[at]) {
			memcpy(one[at], nodes[i].path.p, nodes[i].path.n + 1);
		}
		at++;
	}
	free(nodes);
	one[at++] = shclw_first(d, "tv.g", NULL);
	one[at++] = shclw_last(d, "u.v.w", NULL);
	one[at++] = shclw_last(d, "n", NULL);
	shclw_free(d);
	CHECK(at == 7);
	for (int which = 0; which < 3; which++) {
		for (size_t i = 0; i < at; i++) {
			d = parse_z(several);
			CHECK(one[i] && edit(d, which, one[i]) == SHCLW_OK);
			// A put adds or changes a line, and a comment on a new name adds 2.
			// Two instances changed are 2 places, with the lines between them in
			// the hunk.
			size_t len = 0;
			char *t = shclw_text(d, &len);
			size_t gone = 0;
			size_t added = 0;
			CHECK(t && hunk(several, t, &gone, &added) && gone <= 1 && added <= 2);
			free(t);
			shclw_free(d);
		}
	}
	for (size_t i = 0; i < at; i++) {
		free(one[i]);
	}

	static const char *const all[] = {"n", "tv.g", "u.v", "u.v.w"};
	for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
		d = parse_z(several);
		size_t want = shclw_count(d, all[i]);
		CHECK(shclw_remove(d, all[i]) == want && want > 0 && !shclw_has(d, all[i]));
		shclw_free(d);
	}
}

// Edits leave what they replaced in shcl's write memory, and a save gives it
// back, so a program that edits and saves in a loop stays level.
static void test_saves_give_back(void) {
#if defined(__GLIBC__) && !defined(UNDER_ASAN)
	enum { BIG = 16 * 1024 };
	char *value = (char *)malloc(BIG);
	CHECK(value != NULL);
	if (!value) {
		return;
	}
	memset(value, 'x', BIG);
	shclw_doc *d = parse_z("# note\nk: 1\nn: 2\n");
	struct mallinfo2 before = mallinfo2();
	for (int i = 0; i < 201; i++) {
		if (i == 1) {
			before = mallinfo2();
		}
		value[0] = (char)('a' + i % 26);
		CHECK(shclw_put_str(d, "k", value, BIG, NULL) == SHCLW_OK);
		CHECK(shclw_write(d, in_scratch("loop.shcl"), NULL) == SHCLW_OK);
	}
	struct mallinfo2 after = mallinfo2();
	size_t grew = after.uordblks > before.uordblks ? after.uordblks - before.uordblks : 0;
	if (grew > (size_t)256 * 1024) {
		(void)fprintf(stderr, "200 puts and saves kept %zu bytes\n", grew);
	}
	CHECK(grew <= (size_t)256 * 1024);
	// The kept lines and the load's own lines are still there.
	CHECK(shclw_line(d, "n") == 3);
	size_t len = 0;
	char *t = shclw_text(d, &len);
	CHECK(t && !strncmp(t, "# note\nk: ", 10) && len > BIG);
	free(t);
	free(value);
	shclw_free(d);
#else
	(void)puts("api: memory over many saves isn't measured in this build");
#endif
}

static void test_compact(void) {
	shclw_doc *d = parse_z("# note\nk: 1\nn: 2\n");
	size_t len = 0;
	// What a read handed out is the caller's, so it outlives a compaction.
	char *k = shclw_str(d, "k", "", 0, &len);
	shclw_compact(d);
	CHECK(k && len == 1 && k[0] == '1');
	free(k);
	CHECK(shclw_line(d, "n") == 3 && shclw_int(d, "n", 0) == 2);
	shclw_compact(NULL);
#if defined(__GLIBC__) && !defined(UNDER_ASAN)
	enum { BIG = 16 * 1024 };
	char *value = (char *)malloc(BIG);
	CHECK(value != NULL);
	if (!value) {
		shclw_free(d);
		return;
	}
	memset(value, 'x', BIG);
	struct mallinfo2 before = mallinfo2();
	for (int i = 0; i < 201; i++) {
		if (i == 1) {
			before = mallinfo2();
		}
		value[0] = (char)('a' + i % 26);
		CHECK(shclw_put_str(d, "k", value, BIG, NULL) == SHCLW_OK);
		shclw_compact(d);
	}
	struct mallinfo2 after = mallinfo2();
	size_t grew = after.uordblks > before.uordblks ? after.uordblks - before.uordblks : 0;
	if (grew > (size_t)256 * 1024) {
		(void)fprintf(stderr, "200 puts with no save kept %zu bytes\n", grew);
	}
	CHECK(grew <= (size_t)256 * 1024);
	CHECK(shclw_line(d, "n") == 3);
	char *t = shclw_text(d, &len);
	CHECK(t && !strncmp(t, "# note\nk: ", 10) && len > BIG);
	free(t);
	free(value);
#else
	(void)puts("api: memory over many edits isn't measured in this build");
#endif
	shclw_free(d);
}

static void path_in(char *out, size_t cap, const char *dir, const char *name) {
	CHECK(snprintf(out, cap, "%s/%s", dir, name) < (int)cap);
}

// A folder of its own under scratch, which drop_dir takes away again.
static void make_dir(char *out, size_t cap, const char *name) {
	path_in(out, cap, scratch, name);
	CHECK(new_folder(out) == 0);
}

static size_t count_dir(const char *dir) {
	DIR *d = opendir(dir);
	size_t n = 0;
	for (struct dirent *e = d ? readdir(d) : NULL; e; e = readdir(d)) {
		n += strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0;
	}
	if (d) {
		(void)closedir(d);
	}
	return n;
}

static void drop_dir(const char *dir) {
	DIR *d = opendir(dir);
	for (struct dirent *e = d ? readdir(d) : NULL; e; e = readdir(d)) {
		if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) {
			char path[8192];
			path_in(path, sizeof path, dir, e->d_name);
			CHECK(unlink(path) == 0 || rmdir(path) == 0);
		}
	}
	if (d) {
		(void)closedir(d);
	}
	CHECK(rmdir(dir) == 0);
}

static void write_mode(const char *path, const char *text, mode_t mode) {
	FILE *f = fopen(path, "wb");
	CHECK(f != NULL);
	if (f) {
		CHECK(fwrite(text, 1, strlen(text), f) == strlen(text));
		CHECK(fclose(f) == 0);
	}
	CHECK(chmod(path, mode) == 0);
}

static bool file_is(const char *path, const char *want) {
	char *text = NULL;
	size_t len = 0;
	bool same = shclw_read_text(path, &text, &len, NULL) == SHCLW_OK && len == strlen(want) && !memcmp(text, want, len);
	free(text);
	return same;
}

static mode_t mode_of(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 ? st.st_mode & 0777 : 0;
}

static bool mode_is(const char *path, mode_t want) { return !HAS_MODES || mode_of(path) == want; }

// What the replacer's steps do, per step of test_replace_read.
typedef struct {
	char bak[8192];
	const char *was;
	int backup;
	int write;
	bool backed_up;
} replace_test;

enum { BACKUP_OK, BACKUP_EDITS, BACKUP_TAKEN };
enum { WRITE_OK, WRITE_FAILS, WRITE_TAKES_FILE, WRITE_HALF };

static shclw_kind test_backup(void *ctx, char **name, shclw_error *err) {
	replace_test *t = (replace_test *)ctx;
	t->backed_up = true;
	if (t->backup == BACKUP_TAKEN) {
		return shclw_i_fail(err, SHCLW_ERR_BACKUP_TAKEN, t->bak, NULL, 0);
	}
	write_mode(t->bak, t->was, 0640);
	if (t->backup == BACKUP_EDITS) {
		char path[8192];
		CHECK(snprintf(path, sizeof path, "%.*s/c.shcl", (int)(strrchr(t->bak, '/') - t->bak), t->bak) > 0);
		write_mode(path, "a: 3\n", 0640);
	}
	*name = shclw_i_dup(t->bak, strlen(t->bak), NULL);
	return shclw_i_ok(err);
}

static shclw_kind test_write(void *ctx, const char *path, const char *data, size_t len, shclw_error *err) {
	const replace_test *t = (const replace_test *)ctx;
	switch (t->write) {
	case WRITE_FAILS: return shclw_i_fail(err, SHCLW_ERR_IO, path, "boom", 0);
	case WRITE_TAKES_FILE: CHECK(unlink(path) == 0); return shclw_i_fail(err, SHCLW_ERR_IO, path, "boom", 0);
	case WRITE_HALF: write_mode(path, "a:", 0640); return shclw_i_fail(err, SHCLW_ERR_IO, path, "boom", 0);
	default: break;
	}
	FILE *f = fopen(path, "wb");
	CHECK(f && fwrite(data, 1, len, f) == len);
	if (f) {
		CHECK(fclose(f) == 0);
	}
	return shclw_i_ok(err);
}

// A save goes to a temp file that's renamed into place, and keeps the mode a
// file had.
static void test_save(void) {
	char dir[4200], path[8192];
	make_dir(dir, sizeof dir, "write");
	path_in(path, sizeof path, dir, "c.shcl");
	shclw_doc *d = parse_z("a: 1\n");
	mode_t was = swap_umask(022);
	CHECK(shclw_write(d, path, NULL) == SHCLW_OK && file_is(path, "a: 1\n") && mode_is(path, 0644));
	CHECK(chmod(path, 0600) == 0 && shclw_put_int(d, "a", 2, NULL) == SHCLW_OK);
	CHECK(shclw_write(d, path, NULL) == SHCLW_OK && file_is(path, "a: 2\n") && mode_is(path, 0600));
	(void)swap_umask(was);
	CHECK(count_dir(dir) == 1);

	// A folder in the way stays, and so does the file beside it.
	char sub[8300];
	path_in(sub, sizeof sub, dir, "sub");
	CHECK(new_folder(sub) == 0);
	shclw_error err;
	CHECK(shclw_write(d, sub, &err) == SHCLW_ERR_IO && count_dir(dir) == 2 && count_dir(sub) == 0);
	shclw_free(d);
	drop_dir(sub);
	drop_dir(dir);
}

// Go's TestReplaceRead, step by step.
static void test_replace_read(void) {
	static const char was[] = "a: 1\n", now[] = "a: 2\n";
	static const struct {
		const char *name;
		const char *read;
		int backup;
		int write;
		bool no_backup;
	} steps[] = {
		{"ok", was, BACKUP_OK, WRITE_OK, false},
		{"changed before", "a: 0\n", BACKUP_OK, WRITE_OK, false},
		{"changed during backup", was, BACKUP_EDITS, WRITE_OK, false},
		{"backup fails", was, BACKUP_TAKEN, WRITE_OK, false},
		{"write fails", was, BACKUP_OK, WRITE_FAILS, false},
		{"write fails and takes the file", was, BACKUP_OK, WRITE_TAKES_FILE, false},
		{"write fails part way", was, BACKUP_OK, WRITE_HALF, false},
		{"no backup", was, BACKUP_OK, WRITE_OK, true},
	};
	for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
		char dir[4200], path[8192];
		char sub[32];
		(void)snprintf(sub, sizeof sub, "replace%zu", i);
		make_dir(dir, sizeof dir, sub);
		path_in(path, sizeof path, dir, "c.shcl");
		write_mode(path, was, 0640);
		replace_test t;
		memset(&t, 0, sizeof t);
		path_in(t.bak, sizeof t.bak, dir, "c.bak");
		t.was = was;
		t.backup = steps[i].backup;
		t.write = steps[i].write;
		shclw_i_replacer how = {steps[i].no_backup ? NULL : test_backup, test_write, &t};
		char *name = NULL;
		shclw_error err;
		shclw_kind k =
			shclw_i_replace_read(path, steps[i].read, strlen(steps[i].read), now, strlen(now), &how, &name, &err);
		bool ok = false;
		switch (i) {
		case 0: ok = k == SHCLW_OK && name && file_is(path, now) && file_is(name, was); break;
		case 1: ok = k == SHCLW_ERR_CHANGED && !t.backed_up && file_is(path, was) && count_dir(dir) == 1; break;
		case 2: ok = k == SHCLW_ERR_CHANGED && file_is(path, "a: 3\n") && count_dir(dir) == 1; break;
		case 3: ok = k == SHCLW_ERR_BACKUP_TAKEN && file_is(path, was) && count_dir(dir) == 1; break;
		case 4: ok = k == SHCLW_ERR_IO && strstr(err.msg, "boom") && file_is(path, was) && count_dir(dir) == 1; break;
		case 5:
			ok = k == SHCLW_ERR_IO && file_is(path, was) && mode_is(path, 0640) && count_dir(dir) == 1 &&
			     strstr(err.msg, "put back");
			break;
		case 6: ok = k == SHCLW_ERR_IO && !name && strstr(err.msg, t.bak) && file_is(t.bak, was); break;
		default: ok = k == SHCLW_OK && !name && file_is(path, now) && count_dir(dir) == 1; break;
		}
		if (!ok) {
			(void)fprintf(stderr, "replace read, %s: %s, %s\n", steps[i].name, shclw_kind_name(k), err.msg);
		}
		CHECK(ok);
		free(name);
		drop_dir(dir);
	}
}

static const char clean2x[] = "tags: a, b\n";
static const char clean2to[] = "tags: [a, b]\n";

// A 2.x file shcl 2.0 reported errors on is refused, with 2.0's errors, and a
// file upgrade leaves it as it was, with no backup.
static void test_upgrade(void) {
	static const struct {
		const char *text;
		size_t line;
	} old[] = {{"a: 1\nb: ```\nraw line\nc: 2\n", 2}, {"x: [ab]\ny: 1\n", 1}};
	for (size_t i = 0; i < sizeof old / sizeof old[0]; i++) {
		const char *text = old[i].text;
		shclw_error err;
		shclw_upgraded *u = NULL;
		CHECK(shclw_upgrade(text, strlen(text), 2, &u, &err) == SHCLW_ERR_REFUSED);
		CHECK(u && u->n_diags > 0 && u->diags[0].line == old[i].line && u->diags[0].severity == SHCLW_SEV_ERROR);
		CHECK(u && !u->changed && u->from == 2 && u->text.n == strlen(text) && !strcmp(u->text.p, text));
		CHECK(strstr(err.msg, "shcl 2.0 reports errors on 1 lines") != NULL);
		free(u);

		char dir[4200], path[8192];
		make_dir(dir, sizeof dir, "refused");
		path_in(path, sizeof path, dir, "c.shcl");
		write_mode(path, text, 0644);
		u = NULL;
		CHECK(shclw_upgrade_file(path, 2, &u, &err) == SHCLW_ERR_REFUSED);
		CHECK(u && u->n_diags > 0 && u->text.n == 0 && u->backup.n == 0 && strstr(err.msg, path));
		CHECK(file_is(path, text) && count_dir(dir) == 1);
		free(u);
		drop_dir(dir);
	}

	shclw_upgraded *u = NULL;
	CHECK(shclw_upgrade(clean2x, strlen(clean2x), 2, &u, NULL) == SHCLW_OK);
	CHECK(u && u->changed && u->from == 2 && !strncmp(u->text.p, clean2to, strlen(clean2to)) && !u->n_diags);
	CHECK(u && u->backup.n == 0 && u->backup.p[0] == '\0' && !u->diags);
	free(u);

	// 2.0 has errors on this one too, so with nothing to mark it as 2.x it's
	// current now (2026101613005800).
	// static const char two_ways[] = "x: [ab]\ny: 1\nz: [ab] c\n";
	// CHECK(shclw_upgrade(two_ways, strlen(two_ways), 0, &u, NULL) == SHCLW_ERR_AMBIGUOUS && u != NULL);
	static const char two_ways[] = "p: a,b\ntags: a, b\n";
	CHECK(shclw_upgrade(two_ways, strlen(two_ways), 0, &u, NULL) == SHCLW_ERR_AMBIGUOUS && u != NULL);
	free(u);
	static const char newer[] = "a: 1\n##    Format   9\n";
	CHECK(shclw_upgrade(newer, strlen(newer), 0, &u, NULL) == SHCLW_ERR_TOO_NEW && u && u->from == 9);
	free(u);
	CHECK(shclw_upgrade("a: 1\n", 5, 9, &u, NULL) == SHCLW_ERR_TOO_NEW && u && u->from == 9);
	free(u);
	CHECK(shclw_upgrade(NULL, 0, 0, &u, NULL) == SHCLW_OK && u && !u->changed && u->from == 3 && u->text.n == 0);
	free(u);
}

static void test_upgrade_file(void) {
	char dir[4200], path[8192];
	make_dir(dir, sizeof dir, "real");
	path_in(path, sizeof path, dir, "c.shcl");
	write_mode(path, clean2x, 0600);
	shclw_error err;
	shclw_upgraded *u = NULL;
	CHECK(shclw_upgrade_file(path, 2, &u, &err) == SHCLW_OK);
	CHECK(u && u->changed && u->from == 2 && !strncmp(u->text.p, clean2to, strlen(clean2to)));
	CHECK(u && shclw_format_of(u->text.p, u->text.n) == 3 && file_is(path, u->text.p));
	CHECK(u && !strncmp(u->backup.p, dir, strlen(dir)) && u->backup.p[strlen(dir)] == '/');
	CHECK(u && file_is(u->backup.p, clean2x) && mode_is(u->backup.p, 0600) && mode_is(path, 0600));
	CHECK(count_dir(dir) == 2);
	// the line lists come through the copy that adds the backup's name
	shclw_upgraded *mem = NULL;
	CHECK(shclw_upgrade(clean2x, strlen(clean2x), 2, &mem, NULL) == SHCLW_OK && mem && mem->n_rewritten > 0);
	CHECK(u && mem && u->n_rewritten == mem->n_rewritten && u->n_lost == mem->n_lost &&
	      !memcmp(u->rewritten, mem->rewritten, mem->n_rewritten * sizeof(size_t)));
	free(mem);
	free(u);
	CHECK(shclw_upgrade_file(path, 0, &u, NULL) == SHCLW_OK && u && !u->changed && u->backup.n == 0);
	CHECK(count_dir(dir) == 2);
	free(u);

	static const char newer[] = "a: 1\n##    Format   9\n";
	write_mode(path, newer, 0644);
	CHECK(shclw_upgrade_file(path, 0, &u, &err) == SHCLW_ERR_TOO_NEW && u && u->from == 9 && file_is(path, newer));
	free(u);
	CHECK(shclw_upgrade_file(path, 9, &u, NULL) == SHCLW_ERR_TOO_NEW && u && u->from == 9);
	free(u);
	CHECK(shclw_upgrade_file(in_scratch("missing.shcl"), 2, &u, NULL) == SHCLW_ERR_NOT_FOUND && u && !u->from);
	free(u);
	CHECK(shclw_upgrade_file(dir, 2, &u, &err) == SHCLW_ERR_IO && u && strstr(err.msg, "not a regular file"));
	free(u);
	drop_dir(dir);

	// Something already at the backup's name is never written over. The name
	// has the time in it to the second, so a tick between taking it and the
	// upgrade makes another; go again then.
	make_dir(dir, sizeof dir, "taken");
	path_in(path, sizeof path, dir, "c.shcl");
	bool tried = false;
	for (int i = 0; i < 3 && !tried; i++) {
		write_mode(path, clean2x, 0644);
		char *name = shcl_backup_file_name(path, 2);
		write_mode(name, "mine\n", 0644);
		shclw_kind k = shclw_upgrade_file(path, 2, &u, NULL);
		char *after = shcl_backup_file_name(path, 2);
		if (!strcmp(name, after)) {
			tried = true;
			CHECK(k == SHCLW_ERR_BACKUP_TAKEN && file_is(path, clean2x) && file_is(name, "mine\n"));
			CHECK(count_dir(dir) == 2);
		}
		free(u);
		free(name);
		free(after);
		drop_dir(dir);
		make_dir(dir, sizeof dir, "taken");
	}
	CHECK(tried);
	drop_dir(dir);
}

// shclw_open upgrades a 2.x file in memory and writes nothing. The first save
// over it, by any name for the same file, backs up the original, and later
// ones don't. A file 2.0 reports errors on is refused with 2.0's errors in the
// empty document's diagnostics.
static void test_open_upgrade(void) {
	char dir[4200], path[8192], other[8300];
	make_dir(dir, sizeof dir, "open");
	path_in(path, sizeof path, dir, "c.shcl");
	write_mode(path, clean2x, 0600);
	shclw_doc *d = NULL;
	shclw_error err;
	CHECK(shclw_open(path, &d, &err) == SHCLW_OK);
	CHECK(shclw_upgraded_from(d) == 2 && shclw_rewrites(d));
	size_t n = 0;
	char *name = shclw_backup_name(d, &n);
	CHECK(name && n == 0 && name[0] == '\0');
	free(name);
	shclw_got_strs tags = shclw_get_strs(d, "tags");
	CHECK(tags.status == SHCLW_GOOD && tags.n == 2);
	free(tags.value);
	CHECK(file_is(path, clean2x) && count_dir(dir) == 1);

	path_in(other, sizeof other, dir, "./c.shcl");
	CHECK(shclw_write(d, other, &err) == SHCLW_OK);
	name = shclw_backup_name(d, &n);
	CHECK(name && n > strlen(dir) && !strncmp(name, dir, strlen(dir)) && name[strlen(dir)] == '/');
	CHECK(name && file_is(name, clean2x) && mode_is(name, 0600));
	char *saved = NULL;
	size_t len = 0;
	CHECK(shclw_read_text(path, &saved, &len, NULL) == SHCLW_OK);
	CHECK(saved && !strncmp(saved, clean2to, strlen(clean2to)) && shclw_format_of(saved, len) == 3);
	free(saved);
	CHECK(!shclw_rewrites(d));
	CHECK(shclw_put_int(d, "n", 1, NULL) == SHCLW_OK);
	CHECK(shclw_write(d, path, &err) == SHCLW_OK && count_dir(dir) == 2);
	char *again = shclw_backup_name(d, NULL);
	CHECK(again && name && !strcmp(again, name));
	free(again);
	free(name);
	shclw_free(d);
	CHECK(shclw_open(path, &d, NULL) == SHCLW_OK && shclw_upgraded_from(d) == 0 && shclw_int(d, "n", 0) == 1);
	shclw_free(d);

	// A file that changed since the open read it is left alone.
	drop_dir(dir);
	make_dir(dir, sizeof dir, "open");
	write_mode(path, clean2x, 0644);
	CHECK(shclw_open(path, &d, NULL) == SHCLW_OK);
	write_mode(path, "tags: x, y\n", 0644);
	CHECK(shclw_write(d, path, &err) == SHCLW_ERR_CHANGED && file_is(path, "tags: x, y\n") && count_dir(dir) == 1);
	name = shclw_backup_name(d, NULL);
	CHECK(name && name[0] == '\0');
	free(name);
	shclw_free(d);

	// 2.0 reports errors on this one too. With nothing to mark it as 2.x it's
	// current now, so the same file with a Format line naming 2 is the refused
	// one (2026101613005800).
	// static const char raw[] = "a: 1\nb: ```\nraw line\nc: 2\n";
	// write_mode(path, raw, 0644);
	// CHECK(shclw_open(path, &d, &err) == SHCLW_ERR_REFUSED && strstr(err.msg, "c.shcl"));
	// shclw_diag *ds = shclw_diags(d, &n);
	// CHECK(ds && n == 1 && ds[0].line == 2 && shclw_errors(d) == 1 && shclw_int(d, "a", 7) == 7);
	static const char raw[] = "##    Format   2\na: 1\nb: ```\nraw line\nc: 2\n";
	write_mode(path, raw, 0644);
	CHECK(shclw_open(path, &d, &err) == SHCLW_ERR_REFUSED && strstr(err.msg, "c.shcl"));
	shclw_diag *ds = shclw_diags(d, &n);
	CHECK(ds && n == 1 && ds[0].line == 3 && shclw_errors(d) == 1 && shclw_int(d, "a", 7) == 7);
	free(ds);
	shclw_free(d);
	CHECK(file_is(path, raw) && count_dir(dir) == 1);
	drop_dir(dir);

	// Something already at the backup's name is never written over. Go again
	// on a tick of the clock.
	make_dir(dir, sizeof dir, "open-taken");
	path_in(path, sizeof path, dir, "c.shcl");
	bool tried = false;
	for (int i = 0; i < 3 && !tried; i++) {
		write_mode(path, clean2x, 0644);
		CHECK(shclw_open(path, &d, NULL) == SHCLW_OK);
		char *taken = shcl_backup_file_name(path, 2);
		write_mode(taken, "mine\n", 0644);
		shclw_kind k = shclw_write(d, path, NULL);
		char *after = shcl_backup_file_name(path, 2);
		if (!strcmp(taken, after)) {
			tried = true;
			CHECK(k == SHCLW_ERR_BACKUP_TAKEN && file_is(path, clean2x) && file_is(taken, "mine\n"));
			char *none = shclw_backup_name(d, NULL);
			CHECK(count_dir(dir) == 2 && shclw_upgraded_from(d) == 2 && none && none[0] == '\0');
			free(none);
		}
		shclw_free(d);
		free(taken);
		free(after);
		drop_dir(dir);
		make_dir(dir, sizeof dir, "open-taken");
	}
	CHECK(tried);
	drop_dir(dir);
}

static const char block2x[] = "#\r\n# This config file format is SHCL.\r\n# \"Simple Hierarchical Config Language\"\r\n"
							  "#    Home     https://github.com/jim-collier/shcl\r\n#\r\n";

// block2x with LF line ends, in buf.
static const char *lf_block(char *buf, size_t cap) {
	size_t n = 0;
	for (const char *c = block2x; *c && n + 1 < cap; c++) {
		if (*c != '\r') {
			buf[n++] = *c;
		}
	}
	buf[n] = '\0';
	return buf;
}

// A save of a file with 2.x's block and no Format line puts the current block
// where it sat, with its Format line, so the next open reads what was put
// rather than take the file as 2.x (2026101513330010).
static void test_write_swaps_old_block(void) {
	char dir[4200], path[8192], text[512], lf[256], other[512];
	make_dir(dir, sizeof dir, "swap");
	path_in(path, sizeof path, dir, "c.shcl");
	(void)snprintf(text, sizeof text, "# app\r\nname: demo\r\n\r\n%s\r\nport: 80\r\n", block2x);
	shclw_doc *d = parse_z(text);
	shclw_error err;
	CHECK(shclw_put_str(d, "dir", "C:\\temp", 7, &err) == SHCLW_OK);
	CHECK(shclw_write(d, path, &err) == SHCLW_OK);
	char *got = NULL;
	size_t len = 0;
	CHECK(shclw_read_text(path, &got, &len, NULL) == SHCLW_OK);
	if (got) {
		CHECK(text_is(d, got));
		const char *at = strstr(got, "##\r\n## This config file format is SHCL.");
		const char *port = strstr(got, "port:");
		CHECK(at && port && at < port && shclw_format_of(got, len) == 3);
		CHECK(!strstr(got, "# This config file format is SHCL.\r\n# "));
		size_t lfs = 0, crlfs = 0;
		for (size_t i = 0; i < len; i++) {
			lfs += got[i] == '\n';
			crlfs += got[i] == '\n' && i > 0 && got[i - 1] == '\r';
		}
		CHECK(lfs == crlfs);
	}
	free(got);
	shclw_free(d);
	CHECK(shclw_open(path, &d, NULL) == SHCLW_OK && shclw_upgraded_from(d) == 0);
	shclw_got_str dir_read = shclw_get_str(d, "dir");
	CHECK(dir_read.status == SHCLW_GOOD && dir_read.len == 7 && !memcmp(dir_read.value, "C:\\temp", 7));
	free(dir_read.value);
	shclw_free(d);

	// A block's text in a raw body is the body's.
	(void)snprintf(other, sizeof other, "tmpl:\n\t~~~\n%s\t~~~\n", lf_block(lf, sizeof lf));
	d = parse_z(other);
	CHECK(shclw_write(d, path, NULL) == SHCLW_OK && file_is(path, other));
	shclw_free(d);
	// A Format line already says what the file is.
	(void)snprintf(other, sizeof other, "%s##    Format   3\n", lf);
	d = parse_z(other);
	CHECK(shclw_write(d, path, NULL) == SHCLW_OK && file_is(path, other));
	shclw_free(d);
	drop_dir(dir);
}

// A file with no Format line or 2.x block that has errors on 3.0 and 2.0
// alike is current, with 3.0's errors in its diagnostics. One marked as 2.x
// is still refused (2026101613005800).
static void test_open_fails_both(void) {
	static const char both[] = "a: 1\nb: ```\nraw line\nc: 2\n";
	char dir[4200], path[8192], lf[256], text[512];
	make_dir(dir, sizeof dir, "both");
	path_in(path, sizeof path, dir, "c.shcl");
	write_mode(path, both, 0644);
	shclw_doc *d = NULL;
	shclw_error err;
	CHECK(shclw_open(path, &d, &err) == SHCLW_OK && shclw_upgraded_from(d) == 0 && !shclw_rewrites(d));
	CHECK(shclw_int(d, "a", 0) == 1 && shclw_errors(d) == 1);
	size_t n = 0;
	shclw_diag *ds = shclw_diags(d, &n);
	CHECK(ds && n == 1 && ds[0].line == 2 && !strcmp(ds[0].code.p, "E005"));
	free(ds);
	shclw_free(d);
	CHECK(shclw_open_with(path, SHCLW_STRICT, &d, &err) == SHCLW_ERR_STRICT);
	shclw_free(d);
	shclw_upgraded *u = NULL;
	CHECK(shclw_upgrade(both, strlen(both), 0, &u, NULL) == SHCLW_OK);
	CHECK(u && !u->changed && u->from == 3 && u->text.n == strlen(both) && !strcmp(u->text.p, both) && !u->n_diags);
	free(u);
	CHECK(shclw_upgrade_file(path, 0, &u, NULL) == SHCLW_OK && u && !u->changed && count_dir(dir) == 1);
	free(u);

	CHECK(shclw_upgrade(both, strlen(both), 2, &u, NULL) == SHCLW_ERR_REFUSED);
	free(u);
	const char *marked[] = {lf_block(lf, sizeof lf), "##    Format   2\n"};
	for (size_t i = 0; i < 2; i++) {
		(void)snprintf(text, sizeof text, "%s%s", marked[i], both);
		CHECK(shclw_upgrade(text, strlen(text), 0, &u, NULL) == SHCLW_ERR_REFUSED);
		free(u);
		write_mode(path, text, 0644);
		CHECK(shclw_open(path, &d, NULL) == SHCLW_ERR_REFUSED);
		shclw_free(d);
	}
	drop_dir(dir);
}

#ifdef _WIN32
typedef struct {
	const char *path, *flip, *bad;
	volatile LONG stop;
} flipper;

// A rename there doesn't replace, and fails while the file is open, so a
// failed flip is skipped rather than the end of the loop.
static unsigned __stdcall flip_loop(void *arg) {
	flipper *f = (flipper *)arg;
	for (unsigned n = 0; !InterlockedCompareExchange(&f->stop, 0, 0); n++) {
		const char *t = n % 2 ? clean2x : f->bad;
		FILE *out = fopen(f->flip, "wb");
		if (out) {
			bool ok = fwrite(t, 1, strlen(t), out) == strlen(t);
			ok = fclose(out) == 0 && ok;
			if (ok) {
				(void)MoveFileExA(f->flip, f->path, MOVEFILE_REPLACE_EXISTING);
			}
		}
	}
	return 0;
}
#endif

// An edit made while the upgrade runs must not get past the 2.0 check. The
// file flips between a clean 2.x text and one 2.0 has errors on, so a backup
// holding the bad text means that text was converted unchecked.
static void test_upgrade_raced(void) {
	static const char bad[] = "x: [ab]\ny: 1\n";
	char root[4200];
	make_dir(root, sizeof root, "raced");
	int upgraded = 0;
	for (int i = 0; i < 100; i++) {
		char dir[8192], path[8300], flip[8300];
		char sub[16];
		(void)snprintf(sub, sizeof sub, "%d", i);
		path_in(dir, sizeof dir, root, sub);
		CHECK(new_folder(dir) == 0);
		path_in(path, sizeof path, dir, "c.shcl");
		path_in(flip, sizeof flip, dir, "flip");
		write_mode(path, clean2x, 0644);
#ifdef _WIN32
		flipper f = {path, flip, bad, 0};
		HANDLE h = (HANDLE)_beginthreadex(NULL, 0, flip_loop, &f, 0, NULL);
		CHECK(h != NULL);
		shclw_upgraded *u = NULL;
		shclw_kind k = shclw_upgrade_file(path, 2, &u, NULL);
		if (h) {
			(void)InterlockedExchange(&f.stop, 1);
			(void)WaitForSingleObject(h, INFINITE);
			(void)CloseHandle(h);
		}
#else
		pid_t pid = fork();
		if (pid == 0) {
			for (unsigned n = 0;; n++) {
				const char *t = n % 2 ? clean2x : bad;
				FILE *f = fopen(flip, "wb");
				if (!f || fwrite(t, 1, strlen(t), f) != strlen(t) || fclose(f) != 0 || rename(flip, path) != 0) {
					_exit(0);
				}
			}
		}
		CHECK(pid > 0);
		shclw_upgraded *u = NULL;
		shclw_kind k = shclw_upgrade_file(path, 2, &u, NULL);
		if (pid > 0) {
			(void)kill(pid, SIGKILL);
			(void)waitpid(pid, NULL, 0);
		}
#endif
		if (k == SHCLW_OK && u && u->backup.n) {
			upgraded++;
			if (!file_is(u->backup.p, clean2x)) {
				(void)fprintf(stderr, "round %d: the backup isn't the clean text, so another was converted\n", i);
				CHECK(false);
			}
		}
		free(u);
		drop_dir(dir);
	}
	CHECK(upgraded > 0);
	drop_dir(root);
}

static char *racer_text(int i) {
	char line[16];
	int n = snprintf(line, sizeof line, "v: %d\n", i);
	char *t = (char *)malloc((size_t)n * 5000 + 1);
	if (!t) {
		return NULL;
	}
	for (int k = 0; k < 5000; k++) {
		memcpy(t + (size_t)k * (size_t)n, line, (size_t)n);
	}
	t[(size_t)n * 5000] = '\0';
	return t;
}

#ifdef _WIN32
typedef struct {
	const char *path;
	HANDLE gate;
	int i;
	shclw_kind k;
} racer;

static unsigned __stdcall race_create(void *arg) {
	racer *r = (racer *)arg;
	(void)WaitForSingleObject(r->gate, INFINITE);
	char *t = racer_text(r->i);
	r->k = t ? shclw_i_create_new(r->path, t, strlen(t), 0600, NULL) : SHCLW_ERR_NO_MEMORY;
	free(t);
	return 0;
}

// Each racer's exit code as the fork side gives it: 0 won, 1 exists.
static bool run_racers(const char *path, int n, int *codes) {
	for (int i = 0; i < n; i++) {
		codes[i] = 2;
	}
	racer rs[8];
	HANDLE hs[8];
	HANDLE gate = n <= 8 ? CreateEventA(NULL, TRUE, FALSE, NULL) : NULL;
	if (!gate) {
		return false;
	}
	int started = 0;
	for (; started < n; started++) {
		rs[started].path = path;
		rs[started].gate = gate;
		rs[started].i = started;
		rs[started].k = SHCLW_ERR_IO;
		hs[started] = (HANDLE)_beginthreadex(NULL, 0, race_create, &rs[started], 0, NULL);
		if (!hs[started]) {
			break;
		}
	}
	(void)SetEvent(gate);
	if (started) {
		(void)WaitForMultipleObjects((DWORD)started, hs, TRUE, INFINITE);
	}
	for (int i = 0; i < started; i++) {
		(void)CloseHandle(hs[i]);
		codes[i] = rs[i].k == SHCLW_OK ? 0 : rs[i].k == SHCLW_ERR_EXISTS ? 1 : 2;
	}
	(void)CloseHandle(gate);
	return started == n;
}
#else
static bool run_racers(const char *path, int n, int *codes) {
	for (int i = 0; i < n; i++) {
		codes[i] = 2;
	}
	int gate[2] = {-1, -1};
	if (pipe(gate) != 0) {
		return false;
	}
	pid_t pids[8];
	for (int i = 0; i < n && i < 8; i++) {
		pids[i] = fork();
		if (pids[i] == 0) {
			char go;
			(void)close(gate[1]);
			(void)read(gate[0], &go, 1); // EOF once the parent closes its end
			char *t = racer_text(i);
			shclw_kind k = t ? shclw_i_create_new(path, t, strlen(t), 0600, NULL) : SHCLW_ERR_NO_MEMORY;
			_exit(k == SHCLW_OK ? 0 : k == SHCLW_ERR_EXISTS ? 1 : 2);
		}
	}
	(void)close(gate[0]);
	(void)close(gate[1]);
	bool ok = n <= 8;
	for (int i = 0; i < n && i < 8; i++) {
		int st = 0;
		bool exited = pids[i] > 0 && waitpid(pids[i], &st, 0) == pids[i] && WIFEXITED(st);
		ok = ok && exited;
		codes[i] = exited ? WEXITSTATUS(st) : 2;
	}
	return ok;
}
#endif

// Of several processes creating one name at once, exactly one wins, and the
// file is all of the winner's text, at its mode.
static void test_create_raced(void) {
	enum { racers = 8 };
	char root[4200];
	make_dir(root, sizeof root, "create-raced");
	for (int round = 0; round < 20; round++) {
		char dir[8192], path[8300];
		char sub[16];
		(void)snprintf(sub, sizeof sub, "%d", round);
		path_in(dir, sizeof dir, root, sub);
		CHECK(new_folder(dir) == 0);
		path_in(path, sizeof path, dir, "a.shcl");
		int codes[racers];
		CHECK(run_racers(path, racers, codes));
		int won = -1;
		for (int i = 0; i < racers; i++) {
			int code = codes[i];
			CHECK(code == 0 || code == 1);
			if (code == 0) {
				CHECK(won < 0);
				won = i;
			}
		}
		CHECK(won >= 0);
		char *want = racer_text(won < 0 ? 0 : won);
		CHECK(want && file_is(path, want));
		free(want);
		CHECK(mode_is(path, 0600));
		int entries = 0;
		DIR *d = opendir(dir);
		for (struct dirent *e = d ? readdir(d) : NULL; e; e = readdir(d)) {
			entries += strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0;
		}
		if (d) {
			(void)closedir(d);
		}
		CHECK(entries == 1);
		drop_dir(dir);
	}
	drop_dir(root);
}

// An edit after InfoBlock that names a newer format leaves Text without the
// block, and Write refuses, writing nothing. One naming an older format is
// refused at the call.
#define BLOCK SHCL_GEN_BANNER

static void test_info(void) {
	shclw_doc *d = parse_z("a: 1\n");
	CHECK(shclw_info_block(d, true, NULL) == SHCLW_OK);
	CHECK(text_is(d, "a: 1\n\n" BLOCK));
	static const char line[] = "##    Format   9";
	CHECK(shclw_comment(d, "a", line, sizeof line - 1, NULL) == SHCLW_OK);
	size_t len = 0;
	char *t = shclw_text(d, &len);
	CHECK(t && !strstr(t, "SHCL"));
	free(t);
	shclw_error err;
	CHECK(shclw_write(d, in_scratch("info.shcl"), &err) == SHCLW_ERR_TOO_NEW && strstr(err.msg, "info.shcl"));
	CHECK(access(in_scratch("info.shcl"), F_OK) != 0);
	shclw_free(d);

	d = parse_z("a: 1\n##    Format   2\n");
	CHECK(shclw_info_block(d, false, &err) == SHCLW_ERR_REFUSED && strstr(err.msg, "upgrade it first"));
	CHECK(shclw_stamp_format(d, NULL) == SHCLW_ERR_REFUSED);
	CHECK(text_is(d, "a: 1\n##    Format   2\n"));
	shclw_free(d);

	d = parse_z("a: 1\n");
	CHECK(shclw_stamp_format(d, NULL) == SHCLW_OK && text_is(d, "a: 1\n##    Format   " STR(SHCLW_FORMAT_MAJOR) "\n"));
	CHECK(shclw_get_int(d, "a").line == 1 && shclw_errors(d) == 0);
	shclw_free(d);
}

// A new document has no lines of its own to keep, so it never rewrites. 3.0
// keeps the lines of a load at any strictness.
static void test_rewrites(void) {
	shclw_doc *d = shclw_new();
	CHECK(shclw_put_int(d, "a.b", 1, NULL) == SHCLW_OK && !shclw_rewrites(d));
	shclw_free(d);
	static const char loose[] = "a:   1\n";
	CHECK(shclw_parse_with(loose, sizeof loose - 1, SHCLW_LOOSE, &d, NULL) == SHCLW_OK);
	CHECK(!shclw_rewrites(d));
	shclw_free(d);
	d = parse_z("a: 1\n");
	CHECK(!shclw_rewrites(d));
	shclw_free(d);
}

static void test_check(void) {
	shclw_doc *d = parse_z("port: 80\nportt: 81\nname: x\nname: y\n");
	shclw_doc *schema =
		parse_z("field: port\n\ttype: int\n\tmin: 1\nfield: name\n\ttype: string\n\trepeat: " LIST("0, 10") "\n");
	size_t n = 0;
	shclw_diag *ds = shclw_check(d, schema, &n);
	CHECK(ds && n == 1 && ds[0].line == 2 && !strcmp(ds[0].code.p, "V001"));
	free(ds);
	ds = shclw_diags(d, &n);
	CHECK(ds && n == 1 && !strcmp(ds[0].code.p, "H001"));
	free(ds);
	shclw_drop_declared(schema, d);
	ds = shclw_diags(d, &n);
	CHECK(ds && n == 0);
	free(ds);
	shclw_free(schema);
	shclw_free(d);
}

static void cleanup(void) {
	static const char *const made[] = {"nul0.shcl", "bad.shcl", "newer.shcl", "good.shcl", "strict.shcl", "loop.shcl"};
	// Each test from test_replace_read on takes away its own folders.
	for (size_t i = 0; i < sizeof made / sizeof made[0]; i++) {
		if (unlink(in_scratch(made[i])) != 0 && errno != ENOENT) {
			(void)fprintf(stderr, "couldn't remove %s\n", in_scratch(made[i]));
		}
	}
	if (rmdir(scratch) != 0) {
		(void)fprintf(stderr, "couldn't remove %s\n", scratch);
	}
}

int main(void) {
	const char *tmp = getenv("TMPDIR");
#ifdef _WIN32
	if (!tmp || !*tmp) {
		tmp = getenv("TEMP");
	}
	(void)snprintf(scratch, sizeof scratch, "%s/shclw-c-api-XXXXXX", tmp && *tmp ? tmp : ".");
	if (_mktemp_s(scratch, strlen(scratch) + 1) != 0 || _mkdir(scratch) != 0) {
		perror("mkdtemp");
		return 2;
	}
#else
	(void)snprintf(scratch, sizeof scratch, "%s/shclw-c-api-XXXXXX", tmp && *tmp ? tmp : "/tmp");
	if (!mkdtemp(scratch)) {
		perror("mkdtemp");
		return 2;
	}
#endif
	test_reads();
	test_fallbacks();
	test_nul();
	test_reads_give_back();
	test_errors();
	test_walk();
	test_first_last();
	test_puts();
	test_verify_put();
	test_put_float32();
	test_shortest();
	test_several();
	test_saves_give_back();
	test_compact();
	test_save();
	test_replace_read();
	test_upgrade();
	test_upgrade_file();
	test_upgrade_raced();
	test_create_raced();
	test_open_upgrade();
	test_write_swaps_old_block();
	test_open_fails_both();
	test_info();
	test_rewrites();
	test_check();
	cleanup();
	if (failures) {
		(void)fprintf(stderr, "api: %d failed\n", failures);
		return 1;
	}
	(void)puts("api: ok");
	return 0;
}
