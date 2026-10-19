// Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
// SPDX-License-Identifier: MIT

// shcl 2.0 in a 3.0 build, for the upgrade check. This side names no shcl type,
// since 2.0's shcl.h and 3.0's can't both be in one .c file.

#ifndef SHCLW3_V2_H
#define SHCLW3_V2_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// A document shcl 2.0 parsed. 2.0 ends the program when it runs out of
// memory, as it always has.
typedef struct shclw_i_v2_doc shclw_i_v2_doc;

shclw_i_v2_doc *shclw_i_v2_parse(const char *text, size_t len);
void shclw_i_v2_free(shclw_i_v2_doc *d);

// Called once per error, in shcl 2.0's order. code is NUL-terminated; msg
// has its length.
typedef void (*shclw_i_v2_each)(void *ctx, size_t line, const char *code, const char *msg, size_t msg_len);

// The errors shcl 2.0 reports, through each, and how many.
size_t shclw_i_v2_errors(shclw_i_v2_doc *d, shclw_i_v2_each each, void *ctx);

// The line of the one field at path, or 0.
size_t shclw_i_v2_line(shclw_i_v2_doc *d, const char *path, size_t plen);

// Called for element i of n of each path 2.0 reads one string array at, in
// order, with the path's line.
typedef void (*shclw_i_v2_elem)(void *ctx, const char *path, size_t plen, size_t line, size_t i, size_t n,
                                const char *v, size_t vlen);

// Every value 2.0 reads at a path with one field, as strings, through each.
void shclw_i_v2_reads(shclw_i_v2_doc *d, shclw_i_v2_elem each, void *ctx);

#ifdef __cplusplus
}
#endif

#endif
