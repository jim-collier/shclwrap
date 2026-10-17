# Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
# SPDX-License-Identifier: MIT
#
# Builds and tests the C wrapper. SHCL_DIR is the folder holding the shcl.h
# to build against, and SHCLW_SHCL its version: 1, 2, or 3, the default.
# 1.2's and 2.0's headers can't be told apart, so it has to be said. On 3.0,
# SHCL2_DIR is the folder with shcl 2.0's, which the upgrade checks old files
# with. cicd/cicd.bash fetches the pinned ones.
#
#   make SHCL_DIR=... [SHCLW_SHCL=1|2] [SHCL2_DIR=...] test     API tests: sanitized, plain, and built as C++
#   make SHCL_DIR=... [SHCLW_SHCL=1|2] [SHCL2_DIR=...] drive    the sanitized shclw-drive, for the shared cases
#   make SHCL_DIR=... [SHCLW_SHCL=1|2] [SHCL2_DIR=...] strict   compile only, with gcc-15 at C17
#   make SHCL_DIR=... [SHCLW_SHCL=1|2] [SHCL2_DIR=...] lint     clang-tidy
#   make SHCL1_DIR=... SHCL2_DIR=... SHCL3_DIR=... pick         a wrong SHCLW_SHCL fails to compile
#   make format-check | format

SHCLW_SHCL ?= 3
SHCL_DIR ?=
SHCL2_DIR ?=
ifneq ($(filter-out format format-check clean pick,$(or $(MAKECMDGOALS),all)),)
ifeq ($(SHCL_DIR),)
$(error set SHCL_DIR to the folder with shcl.h)
endif
ifeq ($(filter 1 2 3,$(SHCLW_SHCL)),)
$(error SHCLW_SHCL is 1, 2, or 3)
endif
ifeq ($(SHCLW_SHCL)$(SHCL2_DIR),3)
$(error set SHCL2_DIR to the folder with shcl 2.0's shcl.h)
endif
endif
# Each version in its own folder, since a change of shcl.h or SHCLW_SHCL
# wouldn't rebuild an object made with the other.
OUT      ?= build/v$(SHCLW_SHCL)
CC       ?= gcc
CXX      ?= g++
CC15     ?= gcc-15
CLANG    ?= clang
CLANG_TIDY ?= clang-tidy
CLANG_FORMAT ?= clang-format

LIB   := shclw.c info.c shclw$(SHCLW_SHCL).c run.c
# 3.0 needs no SHCLW_SHCL, and the build leaves it out to show that.
PICK  := $(if $(filter 3,$(SHCLW_SHCL)),,-DSHCLW_SHCL=$(SHCLW_SHCL))
WARN  := -Wall -Wextra -Wpedantic -Wshadow -Wvla -Wconversion -Werror
CFLAGS_ALL := -std=c11 -O1 -g $(WARN) $(PICK) -I. -I$(SHCL_DIR)
CXXFLAGS_ALL := -std=c++20 -O1 -g -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror $(PICK) -I. -I$(SHCL_DIR)
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

.PHONY: all test drive drive-plain strict lint analyze analyze-ctu names pick format format-check clean
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

# Only 3.0 has shcl 2.0 built in beside it.
IS3  := $(filter 3,$(SHCLW_SHCL))
objs = $(addprefix $(OUT)/$(1)/,$(LIB:.c=.o) $(if $(IS3),shclw3_v2.o))
impl = $(OUT)/$(1)/shcl_impl.o $(if $(IS3),$(OUT)/$(1)/shclw3_v2_impl.o)

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

test: $(if $(IS3),names) $(OUT)/api-test-asan $(OUT)/api-test-plain $(OUT)/api-test-cxx
	LSAN_OPTIONS=suppressions=$(CURDIR)/lsan.supp:print_suppressions=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 $(OUT)/api-test-asan
	$(OUT)/api-test-plain
	$(OUT)/api-test-cxx

# A 2.0 name left as it was would meet 3.0's at link time, or, with no 3.0
# twin, hide in the program as shcl's.
names: $(OUT)/plain/shclw3_v2.o $(OUT)/plain/shclw3_v2_impl.o
	@if nm -g --defined-only $^ | grep -E ' _?shcl_'; then echo "shcl 2.0 names not in shclw3_v2_names.h" >&2; exit 1; fi

strict:
	$(CC15) -std=c17 $(WARN) $(PICK) -I. -I$(SHCL_DIR) -fsyntax-only $(LIB) shclw-drive.c tests/api_test.c
	$(if $(IS3),$(CC15) -std=c17 $(WARN) -I. -I$(SHCL2_DIR) -fsyntax-only shclw3_v2.c)

lint:
	$(CLANG_TIDY) --quiet $(LIB) shclw-drive.c tests/api_test.c -- -std=c11 $(PICK) -I. -I$(SHCL_DIR)
	$(if $(IS3),$(CLANG_TIDY) --quiet shclw3_v2.c -- -std=c11 -I. -I$(SHCL2_DIR))

# Analyzer diagnostics must be clean; -Werror makes both compiler frontends fail.
ANALYZE_C := $(LIB) shclw-drive.c tests/api_test.c tests/pick.c
analyze:
	@set -e; for file in $(ANALYZE_C); do \
		$(CC15) -std=c17 -O1 -g $(WARN) -Werror $(PICK) -I. -I$(SHCL_DIR) -fanalyzer -c -o /dev/null "$$file"; \
		$(CLANG) --analyze -std=c11 -O1 -g $(WARN) -Werror $(PICK) -I. -I$(SHCL_DIR) -Xanalyzer -analyzer-output=text -o /dev/null "$$file"; \
	done
	$(CC15) -std=c17 -O1 -g $(WARN) -Werror -isystem $(SHCL_DIR) -I. -fanalyzer -c -o /dev/null tests/shcl_impl.c
	$(CLANG) --analyze -std=c11 -O1 -g $(WARN) -Werror -isystem $(SHCL_DIR) -I. -Xanalyzer -analyzer-output=text -o /dev/null tests/shcl_impl.c
	$(if $(IS3),$(CC15) -std=c17 -O1 -g $(WARN) -Werror -I. -isystem $(SHCL2_DIR) -fanalyzer -c -o /dev/null shclw3_v2.c && $(CLANG) --analyze -std=c11 -O1 -g $(WARN) -Werror -I. -isystem $(SHCL2_DIR) -Xanalyzer -analyzer-output=text -o /dev/null shclw3_v2.c && $(CC15) -std=c17 -O1 -g $(WARN) -Werror -I. -isystem $(SHCL2_DIR) -fanalyzer -c -o /dev/null shclw3_v2_impl.c && $(CLANG) --analyze -std=c11 -O1 -g $(WARN) -Werror -I. -isystem $(SHCL2_DIR) -Xanalyzer -analyzer-output=text -o /dev/null shclw3_v2_impl.c)

# Each shcl.h with each SHCLW_SHCL, or none, in C and C++: only the right one
# compiles. 1.2's and 2.0's headers define the same macros, so a wrong pick
# has to fail on what they declare.
PICKS := 1:1:ok 1:2:no 1:3:no 1:none:no 2:2:ok 2:1:no 2:3:no 2:none:no 3:none:ok 3:3:ok 3:1:no 3:2:no
pick_field = $(word $(1),$(subst :, ,$(2)))
define pick_one
	@if $(1) -fsyntax-only -I. -I$(SHCL$(call pick_field,1,$(2))_DIR) \
		$(if $(filter none,$(call pick_field,2,$(2))),,-DSHCLW_SHCL=$(call pick_field,2,$(2))) tests/pick.c 2>/dev/null; \
		then got=ok; else got=no; fi; \
	if [ "$$got" != $(call pick_field,3,$(2)) ]; then \
		echo "shcl.h $(call pick_field,1,$(2)) with SHCLW_SHCL $(call pick_field,2,$(2)): $$got" >&2; exit 1; fi

endef
pick:
	$(if $(and $(SHCL1_DIR),$(SHCL2_DIR),$(SHCL3_DIR)),,$(error set SHCL1_DIR, SHCL2_DIR, and SHCL3_DIR))
	$(foreach p,$(PICKS),$(call pick_one,$(CC) -std=c11 $(WARN),$(p)))
	$(foreach p,$(PICKS),$(call pick_one,$(CXX) -x c++ -std=c++20 $(WARN),$(p)))

format:
	$(CLANG_FORMAT) -i $(FORMATTED)
format-check:
	$(CLANG_FORMAT) --dry-run -Werror $(FORMATTED)

# The default OUT is a folder per version under build.
clean:
	rm -rf -- $(if $(filter build/v%,$(OUT)),build,$(OUT))
