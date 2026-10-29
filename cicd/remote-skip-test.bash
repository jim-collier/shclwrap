#!/usr/bin/env bash

#  shellcheck disable=1091  ## 'Not following.' The include is found at run time.
#  shellcheck disable=2016  ## 'Expressions don't expand in single quotes.' They're the stand-ins' own text.

##	Purpose: Test dejey. remote-test.bash gives a SKIP line and passes when a
##		test box can't be used: no wrap helper, a lock it can't take, or a box
##		that doesn't answer ssh. Stand-ins for the wrap and ssh, so no box is
##		touched.
##	Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
##	SPDX-License-Identifier: MIT

set -Eeuo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${root}/cicd/utility/include/safe-rm.bash"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/test_shclw_remote_XXXXXX")"
touch "${scratch}/.shclw-remote-owned"
fCleanup(){
	if [[ -f "${scratch}/.shclw-remote-owned" ]]; then fSafeRm "${scratch}" >/dev/null; fi
	return 0
}
trap fCleanup EXIT
fails=0
fFail(){ echo "dejey: $*" >&2; fails=$((fails + 1)); }

## A wrap that runs what follows its "--", one that can't take the lock, and an ssh that gets no answer.
mkdir "${scratch}/bin"
printf '%s\n' '#!/usr/bin/env bash' 'while (($#)) && [[ "$1" != -- ]]; do shift; done; shift; exec "$@"' > "${scratch}/wrap-runs"
printf '%s\n' '#!/usr/bin/env bash' 'echo "in line for $2" >&2; exit 3' > "${scratch}/wrap-busy"
printf '%s\n' '#!/usr/bin/env bash' 'exit 255' > "${scratch}/bin/ssh"
chmod +x "${scratch}/wrap-runs" "${scratch}/wrap-busy" "${scratch}/bin/ssh"

## fCase NAME WRAP WANT: one run, which has to pass with WANT as its line.
fCase(){
	local name="$1" wrap="$2" want="$3" out rc=0 work="${scratch}/work-${1}"
	mkdir "${work}"
	out="$(PATH="${scratch}/bin:${PATH}" SHCLW_VM_WRAP="${wrap}" SHCLW_REMOTE_WAIT=1 bash "${root}/cicd/remote-test.bash" "${work}" windows 2>&1)" || rc=$?
	((rc == 0)) || fFail "${name}: exited ${rc}: ${out}"
	grep -qxF -- "${want}" <<<"${out}" || fFail "${name}: no '${want}' line: ${out}"
	if grep -qE '^(PASS|FAIL) ' <<<"${out}"; then fFail "${name}: a PASS or FAIL line with no box: ${out}"; fi
	return 0
}
fCase nohelper "${scratch}/none"      "SKIP dejex Windows tests: no host lock wrap helper here"
fCase busy     "${scratch}/wrap-busy" "SKIP dejex Windows tests: no answer from b29w"
fCase away     "${scratch}/wrap-runs" "SKIP dejex Windows tests: no answer from b29w"

((fails == 0)) || exit 1
echo "PASS dejey a test box that's away or can't be held is a SKIP, and the run goes on"
