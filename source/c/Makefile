# Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
# SPDX-License-Identifier: MIT
#
# Builds and tests the C wrapper. SHCL_DIR is the folder holding shcl 3.0's
# shcl.h, and SHCL2_DIR the one with shcl 2.0's, which the upgrade checks old
# files with. cicd/cicd.bash fetches the pinned ones.
#
#   make SHCL_DIR=... SHCL2_DIR=... test     API tests: sanitized, plain, and built as C++
#   make SHCL_DIR=... SHCL2_DIR=... drive    the sanitized shclw-drive, for the shared cases
#   make SHCL_DIR=... SHCL2_DIR=... strict   compile only, with gcc-15 at C17
#   make SHCL_DIR=... SHCL2_DIR=... lint     clang-tidy
#   make SHCL_DIR=... SHCL2_DIR=... analyze-shcl   the analyzers over shcl itself, held to shcl-known/
#   make format-check | format

SHCL_DIR ?=
SHCL2_DIR ?=
ifneq ($(filter-out format format-check clean,$(or $(MAKECMDGOALS),all)),)
ifeq ($(SHCL_DIR),)
$(error set SHCL_DIR to the folder with shcl.h)
endif
ifeq ($(SHCL2_DIR),)
$(error set SHCL2_DIR to the folder with shcl 2.0's shcl.h)
endif
endif
OUT      ?= build
CC       ?= gcc
CXX      ?= g++
CC15     ?= gcc-15
CLANG    ?= clang
CLANG_TIDY ?= clang-tidy
CLANG_FORMAT ?= clang-format

LIB   := shclw.c info.c shclw3.c run.c
WARN  := -Wall -Wextra -Wpedantic -Wshadow -Wvla -Wconversion -Werror
CFLAGS_ALL := -std=c11 -O1 -g $(WARN) -I. -I$(SHCL_DIR)
CXXFLAGS_ALL := -std=c++20 -O1 -g -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror -I. -I$(SHCL_DIR)
SAN   := -fsanitize=address,leak,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
# shcl is first-party in its own repo; here it's built as a program would.
SHCL_FLAGS := -std=c11 -O1 -g -I$(SHCL_DIR)
# shcl 2.0, renamed, for the upgrade check. Its 2 files see 2.0's shcl.h and
# never 3.0's.
V2_FLAGS := -std=c11 -O1 -g $(WARN) -I. -I$(SHCL2_DIR)
V2_CXXFLAGS := -std=c++20 -O1 -g -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror -I. -I$(SHCL2_DIR)
SHCL2_FLAGS := -std=c11 -O1 -g -I$(SHCL2_DIR)
V2_DEPS := shclw3_v2.h shclw3_v2_names.h $(SHCL2_DIR)/shcl.h

FORMATTED := $(wildcard *.c *.h tests/*.c)

.PHONY: all test drive drive-plain strict lint analyze analyze-shcl analyze-shcl-report analyze-ctu names format format-check clean FORCE
all: test drive strict

$(OUT)/asan $(OUT)/plain $(OUT)/cxx:
	mkdir -p $@

$(OUT)/asan/%.o: %.c *.h $(SHCL_DIR)/shcl.h | $(OUT)/asan
	$(CC) $(CFLAGS_ALL) $(SAN) -c $< -o $@
$(OUT)/asan/%.o: tests/%.c *.h $(SHCL_DIR)/shcl.h | $(OUT)/asan
	$(CC) $(CFLAGS_ALL) $(SAN) -c $< -o $@
$(OUT)/asan/shcl_impl.o: tests/shcl_impl.c $(SHCL_DIR)/shcl.h | $(OUT)/asan
	$(CC) $(SHCL_FLAGS) $(SAN) -c $< -o $@

$(OUT)/plain/%.o: %.c *.h $(SHCL_DIR)/shcl.h | $(OUT)/plain
	$(CC) $(CFLAGS_ALL) -c $< -o $@
$(OUT)/plain/%.o: tests/%.c *.h $(SHCL_DIR)/shcl.h | $(OUT)/plain
	$(CC) $(CFLAGS_ALL) -c $< -o $@
$(OUT)/plain/shcl_impl.o: tests/shcl_impl.c $(SHCL_DIR)/shcl.h | $(OUT)/plain
	$(CC) $(SHCL_FLAGS) -c $< -o $@

$(OUT)/asan/shclw3_v2.o: shclw3_v2.c $(V2_DEPS) | $(OUT)/asan
	$(CC) $(V2_FLAGS) $(SAN) -c $< -o $@
$(OUT)/asan/shclw3_v2_impl.o: shclw3_v2_impl.c $(V2_DEPS) | $(OUT)/asan
	$(CC) $(SHCL2_FLAGS) $(SAN) -c $< -o $@
$(OUT)/plain/shclw3_v2.o: shclw3_v2.c $(V2_DEPS) | $(OUT)/plain
	$(CC) $(V2_FLAGS) -c $< -o $@
$(OUT)/plain/shclw3_v2_impl.o: shclw3_v2_impl.c $(V2_DEPS) | $(OUT)/plain
	$(CC) $(SHCL2_FLAGS) -c $< -o $@
$(OUT)/cxx/shclw3_v2.o: shclw3_v2.c $(V2_DEPS) | $(OUT)/cxx
	$(CXX) -x c++ $(V2_CXXFLAGS) -c $< -o $@

# Every C file of ours again as C++, which is how a C++ program sees shclw.h.
$(OUT)/cxx/%.o: %.c *.h $(SHCL_DIR)/shcl.h | $(OUT)/cxx
	$(CXX) -x c++ $(CXXFLAGS_ALL) -c $< -o $@
$(OUT)/cxx/%.o: tests/%.c *.h $(SHCL_DIR)/shcl.h | $(OUT)/cxx
	$(CXX) -x c++ $(CXXFLAGS_ALL) -c $< -o $@

# shcl 2.0 is built in beside 3.0.
objs = $(addprefix $(OUT)/$(1)/,$(LIB:.c=.o) shclw3_v2.o)
impl = $(OUT)/$(1)/shcl_impl.o $(OUT)/$(1)/shclw3_v2_impl.o

$(OUT)/shclw-drive: $(call objs,asan) $(OUT)/asan/shclw-drive.o $(call impl,asan)
	$(CC) $(SAN) $^ -lm -o $@
$(OUT)/shclw-drive-plain: $(call objs,plain) $(OUT)/plain/shclw-drive.o $(call impl,plain)
	$(CC) $^ -lm -o $@
$(OUT)/api-test-asan: $(call objs,asan) $(OUT)/asan/api_test.o $(call impl,asan)
	$(CC) $(SAN) $^ -lm -o $@
$(OUT)/api-test-plain: $(call objs,plain) $(OUT)/plain/api_test.o $(call impl,plain)
	$(CC) $^ -lm -o $@
$(OUT)/api-test-cxx: $(call objs,cxx) $(OUT)/cxx/api_test.o $(call impl,plain)
	$(CXX) $^ -lm -o $@

drive: $(OUT)/shclw-drive

drive-plain: $(OUT)/shclw-drive-plain

test: names $(OUT)/api-test-asan $(OUT)/api-test-plain $(OUT)/api-test-cxx
	LSAN_OPTIONS=suppressions=$(CURDIR)/lsan.supp:print_suppressions=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 $(OUT)/api-test-asan
	$(OUT)/api-test-plain
	$(OUT)/api-test-cxx

# A 2.0 name left as it was would meet 3.0's at link time, or, with no 3.0
# twin, hide in the program as shcl's.
names: $(OUT)/plain/shclw3_v2.o $(OUT)/plain/shclw3_v2_impl.o
	@if nm -g --defined-only $^ | grep -E ' _?shcl_'; then echo "shcl 2.0 names not in shclw3_v2_names.h" >&2; exit 1; fi

strict:
	$(CC15) -std=c17 $(WARN) -I. -I$(SHCL_DIR) -fsyntax-only $(LIB) shclw-drive.c tests/api_test.c
	$(CC15) -std=c17 $(WARN) -I. -I$(SHCL2_DIR) -fsyntax-only shclw3_v2.c

lint:
	$(CLANG_TIDY) --quiet $(LIB) shclw-drive.c tests/api_test.c -- -std=c11 -I. -I$(SHCL_DIR)
	$(CLANG_TIDY) --quiet shclw3_v2.c -- -std=c11 -I. -I$(SHCL2_DIR)

# Analyzer diagnostics must be clean; -Werror makes both compiler frontends fail.
ANALYZE_C := $(LIB) shclw-drive.c tests/api_test.c
analyze:
	@set -e; for file in $(ANALYZE_C); do \
		$(CC15) -std=c17 -O1 -g $(WARN) -Werror -I. -I$(SHCL_DIR) -fanalyzer -c -o /dev/null "$$file"; \
		$(CLANG) --analyze -std=c11 -O1 -g $(WARN) -Werror -I. -I$(SHCL_DIR) -Xanalyzer -analyzer-output=text -o /dev/null "$$file"; \
	done
	$(CC15) -std=c17 -O1 -g $(WARN) -Werror -I. -isystem $(SHCL2_DIR) -fanalyzer -c -o /dev/null shclw3_v2.c
	$(CLANG) --analyze -std=c11 -O1 -g $(WARN) -Werror -I. -isystem $(SHCL2_DIR) -Xanalyzer -analyzer-output=text -o /dev/null shclw3_v2.c

# shcl's own code, which every program built on this runs. Not -isystem, and
# clang told to look in headers, or neither reports a thing in shcl.h. No style
# warnings, since it isn't ours. Each pin's findings must match its list in
# shcl-known/ exactly, read and judged there (backlog 2026102321270143).
# analyze-shcl-report writes the reports and checks nothing.
SHCL_REPORT ?= $(OUT)/analyze-shcl
SHCL_KNOWN ?= shcl-known/$(notdir $(SHCL_DIR)).txt
SHCL2_KNOWN ?= shcl-known/$(notdir $(SHCL2_DIR)).txt
SHCL_ANALYZED := $(foreach v,shcl3 shcl2,$(SHCL_REPORT)/$(v).gcc.txt $(SHCL_REPORT)/$(v).clang.txt)
$(SHCL_REPORT)/shcl3.%: TU := tests/shcl_impl.c
$(SHCL_REPORT)/shcl3.%: TU_DIR := $(SHCL_DIR)
$(SHCL_REPORT)/shcl2.%: TU := shclw3_v2_impl.c
$(SHCL_REPORT)/shcl2.%: TU_DIR := $(SHCL2_DIR)
$(SHCL_REPORT):
	mkdir -p $@
$(SHCL_REPORT)/%.gcc.txt: FORCE | $(SHCL_REPORT)
	$(CC15) -std=c17 -O1 -g -I. -I$(TU_DIR) -fanalyzer -c -o /dev/null $(TU) >$@ 2>&1 || { cat $@ >&2; exit 1; }
$(SHCL_REPORT)/%.clang.txt: FORCE | $(SHCL_REPORT)
	$(CLANG) --analyze -std=c11 -O1 -g -I. -I$(TU_DIR) -Xanalyzer -analyzer-opt-analyze-headers -Xanalyzer -analyzer-output=text -o /dev/null $(TU) >$@ 2>&1 || { cat $@ >&2; exit 1; }
analyze-shcl-report: $(SHCL_ANALYZED)
analyze-shcl: analyze-shcl-report
	@rc=0; \
	for tool in gcc clang; do \
		python3 ../../cicd/shcl_known.py analyzer $(SHCL_KNOWN) $$tool $(SHCL_REPORT)/shcl3.$$tool.txt || rc=1; \
		python3 ../../cicd/shcl_known.py analyzer $(SHCL2_KNOWN) $$tool $(SHCL_REPORT)/shcl2.$$tool.txt || rc=1; \
	done; exit $$rc
FORCE:

format:
	$(CLANG_FORMAT) -i $(FORMATTED)
format-check:
	$(CLANG_FORMAT) --dry-run -Werror $(FORMATTED)

clean:
	rm -rf -- $(OUT)
