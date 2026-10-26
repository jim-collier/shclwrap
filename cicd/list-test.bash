#!/usr/bin/env bash

#  shellcheck disable=2086  ## FLAGS is split into words on purpose.

##	Purpose: Test deea7. cicd.bash --list names every test ID a run would run,
##		each once, under the step that runs it, and runs nothing. --dry-run
##		stays the step list (2026102507433885).
##	Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
##	SPDX-License-Identifier: MIT

set -Eeuo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${here}/.." && pwd)"
fails=0
fFail(){ echo "FAIL: $*" >&2; fails=$((fails + 1)); }
idRe='^[0-9a-hjkmnp-tv-z]{5} '

fList(){ env -u SHCLW_FUZZ_SECONDS -u SHCLW_GOFUZZ_SECONDS bash "${here}/cicd.bash" --list $1; }
## fIds TEXT: the IDs listed, one a line.
fIds(){ grep -E "${idRe}" <<<"$1" | cut -d' ' -f1 || true; }
## fStepIds TEXT STEP: the IDs listed under STEP.
fStepIds(){ awk -v want="; $2, test IDs ]" 'index($0, want) { on = 1; next } /^\[ / { on = 0 } on' <<<"$1" | grep -E "${idRe}" | cut -d' ' -f1 || true; }

logsBefore="$(find "${root}/cicd/artifacts/lint" -maxdepth 1 -type f 2>/dev/null | sort || true)"
full="$(fList "")"
full_ids="$(fIds "${full}")"

## Every test ID in the tree: cicd's own, the C container's, every language test, every case, and the fuzz.
## steps-test.bash's IDs are made up.
bashFiles=()
for f in "${here}"/*.bash "${here}"/utility/include/*.bash; do
	[[ "${f##*/}" == steps-test.bash || "${f##*/}" == list-test.bash ]] || bashFiles+=("${f}")
done
mapfile -t tree < <(
	grep -ohE '(fTest( -q)?|fTest_Start|fTest_Skip) [a-z0-9]{5} "|\[[a-z0-9]{5}\]="' "${bashFiles[@]}" | grep -oE '[a-z0-9]{5}' | sort -u
	cd "${here}" && PYTHONDONTWRITEBYTECODE=1 python3 -c '
import fuzz, test_ids
from c_cases import case_id
print(*(t.tid for t in test_ids.all_tests()), sep="\n")
print(*(case_id(p) for p in (test_ids.ROOT / "tests" / "cases").iterdir() if p.is_dir()), sep="\n")
print(*(tid for tid, _ in fuzz.TESTS.values()), sep="\n")'
)
((${#tree[@]} > 150)) || fFail "found ${#tree[@]} test IDs in the tree, want over 150"
for id in "${tree[@]}"; do grep -qx -- "${id}" <<<"${full_ids}" || fFail "the default run's --list lacks ${id}"; done
dupes="$(sort <<<"${full_ids}" | uniq -d)"
[[ -z "${dupes}" ]] || fFail "listed more than once: ${dupes//$'\n'/ }"
if grep -q '^----- ' <<<"${full}"; then fFail "a test with no ID: $(grep -m1 '^----- ' <<<"${full}")"; fi

## Each under its own step.
[[ "$(fStepIds "${full}" Lint)" == *de7zp* ]]            || fFail "de7zp isn't under Lint"
[[ "$(fStepIds "${full}" "C leak checks")" == *deb9f* ]] || fFail "deb9f isn't under C leak checks"
[[ "$(fStepIds "${full}" Fuzz)" == *ddzw3* ]]            || fFail "ddzw3 isn't under Fuzz"
[[ "$(fStepIds "${full}" Tests)" == *de07q* ]]           || fFail "de07q isn't under Tests"
[[ "$(fStepIds "${full}" Format)" == "" && "${full}" == *$'Format, test IDs ]\n(none)'* ]] || fFail "Format doesn't say (none)"

## A step that's off lists nothing, and areas pick the languages.
out="$(fList "--no-fuzz --no-leakcheck")"
if grep -qE '^(ddzw1|ddzw3|deb9f|de2xz) ' <<<"${out}"; then fFail "--no-fuzz --no-leakcheck still lists a fuzz or leak test"; fi
[[ "${out}" != *"; Fuzz, test IDs"* ]] || fFail "--no-fuzz still has a Fuzz section"
out="$(fList "--backlog-run-quick go")"
grep -qE '^ddzw5 Go TestOpen$' <<<"${out}"           || fFail "--backlog-run-quick go lacks Go's TestOpen"
grep -qE '^ddzvq case 001-reads \(Go\)$' <<<"${out}" || fFail "--backlog-run-quick go: case 001 isn't Go's alone"
if grep -qE '^[a-z0-9]{5} (Rust|Python|C) |^(de7yv|debab|de07q|decd4) ' <<<"${out}"; then fFail "--backlog-run-quick go lists another area's test"; fi
out="$(fList "--backlog-run-quick rust python c")"
grep -qE '^ddzvq case 001-reads \(Rust, Python, C\)$' <<<"${out}" || fFail "--backlog-run-quick rust python c: case 001 isn't theirs"
if grep -qE '^[a-z0-9]{5} Go |^de7yv ' <<<"${out}"; then fFail "--backlog-run-quick rust python c lists a Go or cicd test"; fi
out="$(fList "--quick")"
if grep -qE '^(de7zp|decas) ' <<<"${out}"; then fFail "--quick lists an analyzer probe it skips"; fi
[[ "$(fStepIds "${out}" Tests)" == *decbj* ]] || fFail "--quick doesn't list the Windows build under Tests"
[[ "$(fStepIds "${out}" Fuzz)" != *ddzw3* ]]  || fFail "--quick lists Go's fuzzer, which it gives 0 seconds"

## --dry-run is the step list alone.
out="$(bash "${here}/cicd.bash" --dry-run)"
if grep -qE "${idRe}" <<<"${out}"; then fFail "--dry-run lists test IDs"; fi

## Nothing ran: no run log, and no scratch folder.
logsAfter="$(find "${root}/cicd/artifacts/lint" -maxdepth 1 -type f 2>/dev/null | sort || true)"
[[ "${logsBefore}" == "${logsAfter}" ]] || fFail "--list wrote a run log"
scratch="$(sed -n 's/^Scratch \.*: //p' <<<"${full}")"
[[ -n "${scratch}" && ! -e "${scratch}" ]] || fFail "--list made its scratch folder '${scratch}'"

if ((fails)); then echo "deea7: ${fails} check(s) failed" >&2; exit 1; fi
echo "PASS deea7 --list names every test a run would run, and runs nothing"


##	History:
##		- 2026-10-25 JC: Created.
