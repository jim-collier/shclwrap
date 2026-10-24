#!/usr/bin/env bash

#  shellcheck disable=2016  ## The grep looks for a literal "${root}".

##	Purpose: Test vGSqu. Two cicd runs in one tree must not overlap, since they
##		share cicd/artifacts/ (2026102323244600). A second run waits for the
##		first, and a script the locked run starts doesn't wait on it.
##	Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
##	SPDX-License-Identifier: MIT

set -Eeuo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lib="${here}/utility/include/run-lock.bash"
[[ -f "${lib}" ]] || { echo "vGSqu: no ${lib}" >&2; exit 1; }
scratch="$(mktemp -d "${TMPDIR:-/tmp}/test_shclw_lock_XXXXXX")"
touch "${scratch}/.shclw-lock-owned"
fCleanup(){
	if [[ -f "${scratch}/.shclw-lock-owned" ]]; then rm -rf -- "${scratch}"; fi
	return 0
}
trap fCleanup EXIT
fFail(){ echo "vGSqu: $*" >&2; exit 1; }

## A stand-in for a cicd run: takes the lock, notes when it starts and ends.
cat > "${scratch}/standin.bash" <<'EOF'
set -Eeuo pipefail
source "$1"
fRunLock "$2" "stand-in $3"
echo "start $3" >> "$2/order"
if [[ "$3" == nest ]]; then bash "$0" "$1" "$2" child 0; fi
sleep "$4"
echo "end $3" >> "$2/order"
EOF
tree="${scratch}/tree"
mkdir -p "${tree}"
fStandin(){ env -u SHCLW_CICD_LOCK bash "${scratch}/standin.bash" "${lib}" "${tree}" "$@"; }

fStandin first 2 2>"${scratch}/first.err" &
firstPid=$!
for ((i = 0; i < 100; i++)); do
	if grep -qx 'start first' "${tree}/order" 2>/dev/null; then break; fi
	sleep 0.1
done
grep -qx 'start first' "${tree}/order" || fFail "the first stand-in run never started"
fStandin second 0 2>"${scratch}/second.err"
wait "${firstPid}" || fFail "the first stand-in run failed: $(cat "${scratch}/first.err")"
order="$(paste -sd ' ' "${tree}/order")"
[[ "${order}" == "start first end first start second end second" ]] || fFail "the runs overlapped: ${order}"
grep -q 'Waiting for the other cicd run in this tree to finish (stand-in first, pid ' "${scratch}/second.err" \
	|| fFail "the second run didn't say what it waited for: $(cat "${scratch}/second.err")"

: > "${tree}/order"
timeout 20 env -u SHCLW_CICD_LOCK bash "${scratch}/standin.bash" "${lib}" "${tree}" nest 0 \
	|| fFail "a script started by the locked run waited on its lock"
order="$(paste -sd ' ' "${tree}/order")"
[[ "${order}" == "start nest start child end child end nest" ]] || fFail "nested order is ${order}"

## Every entry point that writes cicd/artifacts/ takes the lock.
for script in cicd.bash c-checks.bash; do
	grep -qF 'fRunLock "${root}" ' "${here}/${script}" || fFail "${script} doesn't take the run lock"
done
echo "OK: one cicd run per tree at a time (vGSqu)"
