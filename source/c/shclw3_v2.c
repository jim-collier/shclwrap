// Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
// SPDX-License-Identifier: MIT

// The 3.0 upgrade's check against shcl 2.0 (design.md, Format and upgrade).
// Built with 2.0's shcl.h on the include path, and not 3.0's.

#include "shclw3_v2_names.h"

#include "shcl.h"

#include "shclw3_v2.h"

#include <stdlib.h>

#ifdef SHCL_FORMAT_MAJOR
#error "shclw3_v2.c wants shcl 2.0's shcl.h, and this one is 3.0 or newer"
#endif

struct shclw_i_v2_doc {
	shcl_doc *d;
};

static shcl_doc *doc_of(shclw_i_v2_doc *d) { return d->d; }

shclw_i_v2_doc *shclw_i_v2_parse(const char *text, size_t len) {
	shclw_i_v2_doc *d = (shclw_i_v2_doc *)malloc(sizeof *d);
	if (d) {
		d->d = shcl_parse(text, len);
	}
	return d;
}

void shclw_i_v2_free(shclw_i_v2_doc *d) {
	if (d) {
		shcl_free(d->d);
		free(d);
	}
}

size_t shclw_i_v2_errors(shclw_i_v2_doc *v2, shclw_i_v2_each each, void *ctx) {
	shcl_doc *d = doc_of(v2);
	size_t found = 0;
	for (size_t i = 0; i < shcl_diag_count(d); i++) {
		if (shcl_diag_severity(d, i) != SHCL_SEV_ERROR) {
			continue;
		}
		shcl_str msg = shcl_diag_message(d, i);
		each(ctx, shcl_diag_line(d, i), shcl_diag_code(d, i), msg.p, msg.n);
		found++;
	}
	return found;
}

size_t shclw_i_v2_line(shclw_i_v2_doc *d, const char *path, size_t plen) { return shcl_line(doc_of(d), path, plen); }

void shclw_i_v2_reads(shclw_i_v2_doc *v2, shclw_i_v2_elem each, void *ctx) {
	shcl_doc *d = doc_of(v2);
	shcl_str *paths = NULL;
	size_t n = shcl_paths(d, &paths);
	for (size_t k = 0; k < n; k++) {
		const char *p = paths[k].p;
		size_t pn = paths[k].n;
		if (shcl_count(d, p, pn) != 1) {
			continue;
		}
		shcl_read_str_arr a = shcl_read_string_array(d, p, pn);
		if (a.status != SHCL_GOOD) {
			continue;
		}
		size_t line = shcl_line(d, p, pn);
		for (size_t i = 0; i < a.n; i++) {
			each(ctx, p, pn, line, i, a.n, a.values[i].p, a.values[i].n);
		}
	}
}
