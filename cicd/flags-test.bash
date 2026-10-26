#!/usr/bin/env bash

#  shellcheck disable=2086  ## FLAGS is split into words on purpose.

##	Purpose: Test de7yv. Checks which stages each mix of cicd.bash flags turns on,
##		through --dry-run, so nothing is built.
##	Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
##	SPDX-License-Identifier: MIT

set -Eeuo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fails=0

## fExpect "FLAGS" SYNC LEAK FUZZ, each on or off.
fExpect(){
	local flags="$1" plan got want
	plan="$(env -u SHCLW_FUZZ_SECONDS -u SHCLW_GOFUZZ_SECONDS bash "${here}/cicd.bash" --dry-run ${flags})"
	got=""
	[[ "${plan}" == *"Remote sync ..: (skipped)"* ]] && got+="off " || got+="on "
	[[ "${plan}" == *"Leak checks ..: (skipped)"* ]] && got+="off " || got+="on "
	[[ "${plan}" == *"Fuzz .........: (skipped)"* ]] && got+="off"  || got+="on"
	want="$2 $3 $4"
	if [[ "${got}" != "${want}" ]]; then echo "FAIL: '${flags}': sync, leak, fuzz are '${got}', want '${want}'" >&2; fails=$((fails + 1)); fi
	return 0
}

fExpect ""                              on  on  on
fExpect "--no-leakcheck"                on  off on
fExpect "--quiet"                       on  on  on
fExpect "--quiet --no-leakcheck"        on  off on
fExpect "--ci"                          off on  on
fExpect "--ci --leakcheck"              off on  on
fExpect "--leakcheck --ci"              off on  on
fExpect "--quick"                       off off on
fExpect "--quick --leakcheck"           off on  on
fExpect "--ci --quick"                  off off on
fExpect "--ci --quick --leakcheck"      off on  on
fExpect "--leakcheck --no-leakcheck"    on  off on
fExpect "--no-leakcheck --leakcheck"    on  on  on
fExpect "--no-sync --no-fuzz"           off on  off
fExpect "--merge-gate"                  off on  on
fExpect "--backlog-run-full"            off on  on
fExpect "--backlog-run-quick"           off off off
fExpect "--gate"                        off off off
fExpect "--backlog-run-quick go cicd"   off off off
fExpect "--backlog-run-quick --leakcheck" off on off
fExpect "--long"                        on  on  on
fExpect "--slow --quick"                off off on
fExpect "--merge-gate --no-container"   off off on
fExpect "--merge-gate --no-fuzz"        off on  off
fExpect "--list --no-remote --keep-temp" on on  on

## fProfile "FLAGS" on|off: the profile step, which --quick and its named run leave out.
fProfile(){
	local plan got=on
	plan="$(bash "${here}/cicd.bash" --dry-run ${1})"
	if [[ "${plan}" == *"Profile ......: (skipped)"* ]]; then got=off; fi
	[[ "${got}" == "$2" ]] || { echo "FAIL: '$1': profile is ${got}, want $2" >&2; fails=$((fails + 1)); }
	return 0
}
fProfile ""                             on
fProfile "--merge-gate"                 on
fProfile "--backlog-run-full"           on
fProfile "--quick"                      off
fProfile "--quick --leakcheck"          off
fProfile "--backlog-run-quick go"       off
fProfile "--no-profile"                 off
fProfile "--merge-gate --no-container"  on

## fMode "FLAGS" "WANT": WANT is a piece of the Mode line.
fMode(){
	local plan
	plan="$(bash "${here}/cicd.bash" --dry-run ${1})"
	[[ "${plan}" == *"Mode .........: "*"$2"* ]] || { echo "FAIL: '$1': Mode line lacks '$2'" >&2; fails=$((fails + 1)); }
	return 0
}
fMode ""                                "full"
fMode "--quick"                         "quick"
fMode "--merge-gate"                    "merge-gate, full, format check only"
fMode "--ci"                            "merge-gate, full, format check only"
fMode "--backlog-run-full"              "backlog-run-full, full, format check only"
fMode "--backlog-run-quick c cicd"      "backlog-run-quick, quick, format check only, areas: c cicd"
fMode "--gate"                          "backlog-run-quick, quick, format check only"
## The 2 older names say what they stand for.
[[ "$(bash "${here}/cicd.bash" --dry-run --ci)" == *"--ci is another name for --merge-gate"* ]] || { echo "FAIL: --ci doesn't say it's --merge-gate" >&2; fails=$((fails + 1)); }
[[ "$(bash "${here}/cicd.bash" --dry-run --gate)" == *"--gate is another name for --backlog-run-quick"* ]] || { echo "FAIL: --gate doesn't say it's --backlog-run-quick" >&2; fails=$((fails + 1)); }
## The default run still formats in place.
if [[ "$(bash "${here}/cicd.bash" --dry-run 2>&1)" == *"format check only"* ]]; then echo "FAIL: the default run checks format only" >&2; fails=$((fails + 1)); fi
if bash "${here}/cicd.bash" --dry-run --backlog-run-quick perl >/dev/null 2>&1; then echo "FAIL: an unknown area was taken" >&2; fails=$((fails + 1)); fi

if ((fails)); then echo "de7yv: ${fails} flag mix(es) wrong" >&2; exit 1; fi
echo "PASS de7yv cicd flag mixes"
