#!/usr/bin/env bash

#  shellcheck disable=1091  ## 'Not following.' The include is found at run time.

##	Purpose: Test dedta. A run drops cached shcl headers no pin names, and keeps
##		the newest 10 fuzz failure sets it marked as its own (2026101916500100).
##		Anything else in those folders stays.
##	Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
##	SPDX-License-Identifier: MIT

set -Eeuo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${here}/utility/include/safe-rm.bash"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/test_shclw_prune_XXXXXX")"
touch "${scratch}/.shclw-prune-owned"
fCleanup(){
	if [[ -f "${scratch}/.shclw-prune-owned" ]]; then fSafeRm "${scratch}" >/dev/null; fi
	return 0
}
trap fCleanup EXIT
fFail(){ echo "dedta: $*" >&2; exit 1; }

## The real functions, taken from cicd.bash, so the test runs what a run does.
for fn in fShclHeader_Prune fFuzzFail_Prune; do
	body="$(sed -n "/^${fn}(){\$/,/^}\$/p" "${here}/cicd.bash")"
	[[ -n "${body}" ]] || fFail "no ${fn} in cicd.bash"
	eval "${body}"
done
fEcho(){ echo "$*"; }

root="${scratch}/tree"
SHCL2_REV="$(printf '2%.0s' {1..40})"
SHCL3_REV="$(printf '3%.0s' {1..40})"
stale="$(printf 'a%.0s' {1..40})"
held="$(printf 'b%.0s' {1..40})"
cache="${root}/cicd/cache/shcl-c"
mkdir -p "${cache}/${SHCL2_REV}" "${cache}/${SHCL3_REV}" "${cache}/${stale}" "${cache}/${held}" "${cache}/notarev"
for rev in "${SHCL2_REV}" "${SHCL3_REV}" "${stale}" "${held}"; do touch "${cache}/${rev}/shcl.h"; done
touch "${cache}/${stale}/shcl.h.part" "${cache}/${held}/mine.txt" "${cache}/notarev/shcl.h"

out="$(fShclHeader_Prune)"
[[ ! -e "${cache}/${stale}" ]] || fFail "the header no pin names was kept"
[[ -f "${cache}/${SHCL2_REV}/shcl.h" && -f "${cache}/${SHCL3_REV}/shcl.h" ]] || fFail "a pinned header was dropped"
[[ -f "${cache}/${held}/mine.txt" && ! -e "${cache}/${held}/shcl.h" ]] || fFail "an unpinned folder holding another file was removed, or kept its header"
[[ -f "${cache}/notarev/shcl.h" ]] || fFail "a folder not named for a rev was touched"
[[ "${out}" == "Dropped the cached shcl.h at ${stale}, which no pin names" ]] || fFail "header prune said: ${out}"

fuzz="${root}/cicd/artifacts/fuzz"
for ((i = 1; i <= 12; i++)); do
	set="${fuzz}/20261021-1200$(printf '%02d' "${i}")"
	mkdir -p "${set}"; touch "${set}/.shclw-cicd-owned" "${set}/case.txt"
done
mkdir -p "${fuzz}/20261020-000000"; touch "${fuzz}/20261020-000000/case.txt"
fFuzzFail_Prune
[[ ! -e "${fuzz}/20261021-120001" && ! -e "${fuzz}/20261021-120002" ]] || fFail "the 2 oldest marked sets were kept"
for ((i = 3; i <= 12; i++)); do
	[[ -d "${fuzz}/20261021-1200$(printf '%02d' "${i}")" ]] || fFail "marked set ${i} of the newest 10 was removed"
done
[[ -f "${fuzz}/20261020-000000/case.txt" ]] || fFail "an unmarked set was removed"
fFuzzFail_Prune
(($(find "${fuzz}" -mindepth 1 -maxdepth 1 -type d | wc -l) == 11)) || fFail "a second prune changed what the first left"
echo "PASS dedta stale headers and fuzz sets are pruned, and nothing else"
