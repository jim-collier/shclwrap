#!/usr/bin/env bash

##	Purpose: Test dee8b. lint-report.bash doesn't read a path or file name as a
##		warning, such as CodeQL's ExtractionWarnings.ql, and still flags a real
##		warning on a line that names a file (2026102509580000).
##	Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
##	SPDX-License-Identifier: MIT

set -Eeuo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/test_shclw_lintreport_XXXXXX")"
touch "${scratch}/.shclw-lintreport-owned"
fCleanup(){
	if [[ -f "${scratch}/.shclw-lintreport-owned" ]]; then rm -rf -- "${scratch}"; fi
	return 0
}
trap fCleanup EXIT
bad=0

## fCase <want: CLEAN|FLAG> <name> <log line>
fCase(){
	local want="$1" name="$2" log="${scratch}/run_20261025-000000.log" got
	printf '%s\nshclwrap CI/CD: done.\n' "$3" > "${log}"
	got="$(bash "${here}/utility/lint-report.bash" --file "${log}")" || { echo "dee8b ${name}: lint-report failed: ${got}" >&2; bad=1; return 0; }
	if [[ "${got%% *}" != "${want}" ]]; then echo "dee8b ${name}: want ${want}, got: ${got}" >&2; bad=1; fi
	return 0
}

fCase CLEAN "CodeQL query plan"     'Compiling query plan for /work/codeql-queries/cpp/ql/src/Diagnostics/ExtractionWarnings.ql.'
fCase CLEAN "CodeQL cache line"     '[171/183] Found in cache: /work/codeql-queries/cpp/ql/src/Diagnostics/ExtractionWarnings.ql.'
fCase CLEAN "CodeQL results line"   '[27/183 eval 31ms] Evaluation done; writing results to codeql/cpp-queries/Diagnostics/ExtractionWarnings.bqrs.'
fCase CLEAN "advisory db url"       'Fetching advisory database from https://github.com/rustsec/advisory-db.git'
fCase CLEAN "test result line"      'PASS dee8b warnings in paths'
fCase FLAG  "gcc warning on a path" '/work/source/c/shclw3.c:12:3: warning: unused variable [-Wunused-variable]'
fCase FLAG  "warning in a file named for it" 'shclw/warnings.go:5:2: warning: shadowed'
fCase FLAG  "bare rustc warning"    'warning: unused import: std::fs'
fCase FLAG  "our own warning line"  'WARNING: C runs on the host: --no-container'
fCase FLAG  "advisory id"           'ID: RUSTSEC-2024-0001'

((bad)) && exit 1
echo "PASS dee8b lint-report doesn't read a path as a warning"
