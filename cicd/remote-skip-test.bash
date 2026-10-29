#!/usr/bin/env bash

#  shellcheck disable=1091  ## 'Not following.' The include is found at run time.
#  shellcheck disable=2016  ## 'Expressions don't expand in single quotes.' They're the stand-ins' own text.

##	Purpose: Test dejey. remote-test.bash gives a SKIP line and passes when a
##		test box can't be used: no wrap helper, a lock it can't take, a box
##		that doesn't answer ssh, or a Mac without a tool the tests need. Stand-ins for the wrap and ssh, so no box is
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
## A Mac that answers, but has no Python new enough, as remote-macos.bash --prep says it.
mkdir "${scratch}/bin-cant"
printf '%s\n' '#!/usr/bin/env bash' 'if [[ " $* " == *" --prep "* ]]; then echo "CANT no Python 3.11 or newer"; exit 3; fi; exit 0' > "${scratch}/bin-cant/ssh"
chmod +x "${scratch}/wrap-runs" "${scratch}/wrap-busy" "${scratch}/bin/ssh" "${scratch}/bin-cant/ssh"

## fCase NAME TARGET WRAP WANT [BIN]: one run, which has to pass with WANT as its line.
fCase(){
	local name="$1" target="$2" wrap="$3" want="$4" bin="${5:-bin}" out rc=0 work="${scratch}/work-${1}"
	mkdir "${work}"
	out="$(PATH="${scratch}/${bin}:${PATH}" SHCLW_VM_WRAP="${wrap}" SHCLW_REMOTE_WAIT=1 bash "${root}/cicd/remote-test.bash" "${work}" "${target}" 2>&1)" || rc=$?
	((rc == 0)) || fFail "${name}: exited ${rc}: ${out}"
	grep -qxF -- "${want}" <<<"${out}" || fFail "${name}: no '${want}' line: ${out}"
	if grep -qE '^(PASS|FAIL) ' <<<"${out}"; then fFail "${name}: a PASS or FAIL line with no box: ${out}"; fi
	return 0
}
fCase nohelper     windows "${scratch}/none"      "SKIP dejex Windows tests: no host lock wrap helper here"
fCase busy         windows "${scratch}/wrap-busy" "SKIP dejex Windows tests: no answer from b29w"
fCase away         windows "${scratch}/wrap-runs" "SKIP dejex Windows tests: no answer from b29w"
fCase mac-nohelper macos   "${scratch}/none"      "SKIP dejgh macOS tests: no host lock wrap helper here"
fCase mac-busy     macos   "${scratch}/wrap-busy" "SKIP dejgh macOS tests: no answer from b26 vmKt6m"
fCase mac-away     macos   "${scratch}/wrap-runs" "SKIP dejgh macOS tests: no answer from b26 vmKt6m"
fCase mac-cant     macos   "${scratch}/wrap-runs" "SKIP dejgh macOS tests: b26: no Python 3.11 or newer; vmKt6m: no Python 3.11 or newer" bin-cant
[[ ! -e "${scratch}/work-mac-cant/remote/tree.tgz" ]] || fFail "mac-cant: packed the tree for a box that can't run it"

((fails == 0)) || exit 1
echo "PASS dejey a test box that's away or can't be held is a SKIP, and the run goes on"
