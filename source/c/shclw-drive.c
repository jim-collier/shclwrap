// Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
// SPDX-License-Identifier: MIT

// shclw-drive runs an ops script against a document, for the tests that
// compare wrapper languages. The same command line as the Go and Rust ones:
//
//	shclw-drive VERSION DOCFILE [DIR] < SCRIPT
//
// C picks its shcl when it's built, so VERSION has to be that one.

#include "shclw_adapter.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(const char *why, const char *what) {
	(void)fprintf(stderr, "shclw-drive: %s%s\n", why, what ? what : "");
	return 2;
}

static char *read_all(FILE *f, size_t *len) {
	size_t cap = 4096;
	size_t n = 0;
	char *buf = (char *)malloc(cap);
	while (buf) {
		if (n == cap) {
			char *grown = (char *)realloc(buf, cap * 2);
			if (!grown) {
				free(buf);
				return NULL;
			}
			buf = grown;
			cap *= 2;
		}
		size_t want = cap - n;
		size_t got = fread(buf + n, 1, want, f);
		n += got;
		if (got < want) {
			if (ferror(f)) {
				free(buf);
				return NULL;
			}
			break;
		}
	}
	*len = n;
	return buf;
}

static bool soak_rounds(size_t *rounds) {
	const char *text = getenv("SHCLW_SOAK_ROUNDS");
	if (!text) {
		*rounds = 1;
		return true;
	}
	errno = 0;
	char *end = NULL;
	unsigned long long n = strtoull(text, &end, 10);
	size_t count = (size_t)n;
	if (errno || end == text || *end || n == 0 || n > 1000000 || (unsigned long long)count != n) {
		return false;
	}
	*rounds = count;
	return true;
}

int main(int argc, char **argv) {
	if (argc < 3 || argc > 4) {
		return fail("usage: shclw-drive VERSION DOCFILE [DIR] < SCRIPT", NULL);
	}
	char version[8];
	(void)snprintf(version, sizeof version, "%d", SHCLW_SHCL);
	if (strcmp(argv[1], version) != 0) {
		return fail("built for shcl version ", version);
	}
	char *doc = NULL;
	size_t doc_len = 0;
	shclw_error err;
	// codeql[cpp/path-injection] This local CLI intentionally reads the caller-selected path.
	if (shclw_read_text(argv[2], &doc, &doc_len, &err) != SHCLW_OK) {
		return fail(err.msg, NULL);
	}
	size_t script_len = 0;
	char *script = read_all(stdin, &script_len);
	if (!script || !shclw_i_utf8_valid(script, script_len)) {
		free(doc);
		free(script);
		return fail("script is not valid UTF-8, or couldn't be read", NULL);
	}
	size_t rounds = 0;
	if (!soak_rounds(&rounds)) {
		free(doc);
		free(script);
		return fail("SHCLW_SOAK_ROUNDS must be 1 to 1000000", NULL);
	}
	size_t out_len = 0;
	char *out = NULL;
	for (size_t i = 0; i < rounds; i++) {
		size_t current_len = 0;
		char *current = shclw_run(doc, doc_len, script, script_len, argc == 4 ? argv[3] : NULL, &current_len);
		if (!current) {
			free(doc);
			free(script);
			free(out);
			return fail("out of memory", NULL);
		}
		if (!out) {
			out = current;
			out_len = current_len;
		} else if (current_len != out_len || memcmp(current, out, out_len) != 0) {
			free(doc);
			free(script);
			free(out);
			free(current);
			return fail("soak rounds differed", NULL);
		} else {
			free(current);
		}
	}
	if (!out) {
		free(doc);
		free(script);
		return fail("out of memory", NULL);
	}
	free(doc);
	free(script);
	int rc = fwrite(out, 1, out_len, stdout) == out_len && fflush(stdout) == 0 ? 0 : 1;
	free(out);
	return rc;
}
