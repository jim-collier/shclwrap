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
fExpect "--ci"                          on  off off
fExpect "--ci --leakcheck"              on  on  off
fExpect "--leakcheck --ci"              on  on  off
fExpect "--quick"                       off off on
fExpect "--quick --leakcheck"           off on  on
fExpect "--ci --quick"                  off off off
fExpect "--ci --quick --leakcheck"      off on  off
fExpect "--leakcheck --no-leakcheck"    on  off on
fExpect "--no-leakcheck --leakcheck"    on  on  on
fExpect "--no-sync --no-fuzz"           off on  off

if ((fails)); then echo "de7yv: ${fails} flag mix(es) wrong" >&2; exit 1; fi
echo "PASS de7yv cicd flag mixes"
