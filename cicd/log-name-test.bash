#!/usr/bin/env bash

##	Purpose: Test vGTf8. cicd runs started in the same second each get their own
##		run log, and lint-report.bash reads the newest (2026102409544336).
##	Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
##	SPDX-License-Identifier: MIT

set -Eeuo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/test_shclw_logname_XXXXXX")"
touch "${scratch}/.shclw-logname-owned"
fCleanup(){
	if [[ -f "${scratch}/.shclw-logname-owned" ]]; then rm -rf -- "${scratch}"; fi
	return 0
}
trap fCleanup EXIT
fFail(){ echo "vGTf8: $*" >&2; exit 1; }

## A stand-in for the start of a cicd run, teeing its output the same way.
cat > "${scratch}/standin.bash" <<'EOF'
set -Eeuo pipefail
source "$1/utility/include/gfs-rotate.bash"
source "$1/utility/include/run-lock.bash"
fRunLog "$2" stamp logFd
exec > >(tee -a "/dev/fd/${logFd}" >/dev/null) 2>&1
exec {logFd}>&-
echo "run $3"
sleep 0.3
echo "run $3 end"
echo "shclwrap CI/CD: done."
exec >&- 2>&-
wait "$!"
EOF
logs="${scratch}/lint"
runs=4
pids=()
for ((i = 1; i <= runs; i++)); do
	bash "${scratch}/standin.bash" "${here}" "${logs}" "${i}" 2>"${scratch}/err${i}" &
	pids+=("$!")
done
for ((i = 0; i < runs; i++)); do
	wait "${pids[i]}" || fFail "stand-in run $((i + 1)) failed: $(cat "${scratch}/err$((i + 1))")"
done

shopt -s nullglob
files=("${logs}"/run_*.log)
((${#files[@]} == runs)) || fFail "${runs} runs left ${#files[@]} log(s): $(ls "${logs}")"
seen=" "
for f in "${files[@]}"; do
	mapfile -t lines < "${f}"
	k="${lines[0]#run }"
	[[ "${#lines[@]}" == 3 && "${lines[1]}" == "run ${k} end" && "${lines[2]}" == *"CI/CD: done." ]] \
		|| fFail "${f##*/} holds more than one run: $(paste -sd '|' "${f}")"
	[[ "${seen}" != *" ${k} "* ]] || fFail "run ${k} has two logs"
	seen+="${k} "
done

newest="$(printf '%s\n' "${files[@]##*/}" | sed 's/^run_\([0-9-]*\).*/\1/' | sort | tail -n 1)"
report="$(bash "${here}/utility/lint-report.bash" --dir "${logs}")" || fFail "lint-report failed: ${report}"
[[ "${report}" == "CLEAN run_${newest}"* ]] || fFail "lint-report didn't read the newest log, run_${newest}: ${report}"
echo "OK: runs started in the same second get their own logs (vGTf8)"
