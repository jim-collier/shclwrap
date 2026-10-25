#!/usr/bin/env bash

#  shellcheck disable=1090  ## The include's path is worked out at run time.
#  shellcheck disable=2016  ## The greps look for literal "${cpuJobs}" text.
#  shellcheck disable=2154  ## cpuJobs is set by the include.

##	Purpose: Test dedkv. A cicd run takes half the CPUs, a quarter when busy or for
##		a hard step, runs at nice 19 and idle IO, and hands the count to every
##		parallel tool, the C container included (2026102416400030).
##	Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
##	SPDX-License-Identifier: MIT

set -Eeuo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lib="${here}/utility/include/cpu-cap.bash"
fails=0
fFail(){ echo "dedkv: $*" >&2; fails=$((fails + 1)); }
[[ -f "${lib}" ]] || { echo "dedkv: no ${lib}" >&2; exit 1; }
source "${lib}"

## fWant COUNT LOAD MODE JOBS BUSY
fWant(){
	local got
	got="$(fCpuJobs "$1" "$2" "$3")"
	[[ "${got}" == "$4 $5" ]] || fFail "fCpuJobs $1 '$2' '$3' gave '${got}', want '$4 $5'"
	return 0
}
fWant 32 6.11  ""   16 0
fWant 32 16.00 ""   16 0
fWant 32 16.01 ""   8  1
fWant 32 40    ""   8  1
fWant 32 0.50  hard 8  0
fWant 32 40    hard 8  1
fWant 4  0     ""   2  0
fWant 4  2.5   ""   1  1
fWant 3  0     hard 1  0
fWant 1  0     ""   1  0
fWant 1  9     hard 1  1
fWant 0  0     ""   1  0
fWant 8  ""    ""   4  0
fWant 8  1.    ""   4  0
## A load that isn't a number is never evaluated, so it can't run anything.
probe="${TMPDIR:-/tmp}/cpu-cap-test-$$"
fWant 8  "x[\$(touch ${probe})]" "" 4 0
if [[ -e "${probe}" ]]; then rm -f -- "${probe}"; fFail "a load string ran a command"; fi

( fCpuCap hard
  for name in GOMAXPROCS RAYON_NUM_THREADS RUST_TEST_THREADS CARGO_BUILD_JOBS SHCLW_JOBS; do
	[[ "$(printenv "${name}")" == "${cpuJobs}" ]] || { echo "dedkv: ${name} isn't exported as ${cpuJobs}" >&2; exit 1; }
  done ) || fails=$((fails + 1))

## Run in a child, so this test's own priority stays as it was.
nice="$(bash -c 'source "$1"; fLowPriority; ps -o ni= -p "$$"' _ "${lib}")"
[[ "${nice// /}" == 19 ]] || fFail "fLowPriority left nice at '${nice}'"
if command -v ionice >/dev/null; then
	io="$(bash -c 'source "$1"; fLowPriority; ionice -p "$$"' _ "${lib}")"
	[[ "${io}" == idle ]] || fFail "fLowPriority left the IO class at '${io}'"
fi
plan="$(bash "${here}/cicd.bash" --dry-run)"
[[ "${plan}" == *"Priority .....: nice 19"* ]] || fFail "cicd.bash doesn't run at nice 19"
[[ "${plan}" =~ CPU\ cap\ \.+:\ [0-9]+\ of\ [0-9]+\ CPUs ]] || fFail "cicd.bash doesn't show its CPU cap"

## Every entry point lowers its own priority and takes the cap from the include.
for script in cicd.bash c-checks.bash lang-checks.bash; do
	grep -qxF 'source "${here}/utility/include/cpu-cap.bash"' "${here}/${script}" || fFail "${script} doesn't source cpu-cap.bash"
	grep -qx 'fLowPriority' "${here}/${script}" || fFail "${script} doesn't call fLowPriority"
done
## Nothing else works out its own count.
while IFS= read -r hit; do fFail "a count from nproc outside cpu-cap.bash: ${hit}"; done \
	< <(grep -rnE '^[^#]*\bnproc\b' "${here}" --include='*.bash' --include='*.py' --exclude-dir=cache --exclude-dir=artifacts | grep -vF -e 'utility/include/cpu-cap.bash:' -e 'cpu-cap-test.bash:' || true)
## Each parallel tool call names the count: cpuJobs in cicd.bash, and SHCLW_JOBS's copy in the image's script.
fJobsNamed(){   ## fJobsNamed FILE VAR
	local line
	while IFS= read -r line; do
		[[ "${line}" == *"-j \"\${$2}\""* ]] || fFail "${1##*/}: cargo without the job count: ${line}"
	done < <(grep -E '^[^#]*\bcargo (build|test|clippy)' "$1")
	while IFS= read -r line; do
		[[ "${line}" == *"-p \"\${$2}\""* ]] || fFail "${1##*/}: go without the job count: ${line}"
	done < <(grep -E '^[^#]*\bgo (build|test|vet) ' "$1")
	return 0
}
fJobsNamed "${here}/cicd.bash" cpuJobs
fJobsNamed "${here}/lang-in-container.bash" jobs
## Each docker run is capped, low-share, and niced inside.
for script in "${here}"/*.bash; do
	[[ "${script}" != "${BASH_SOURCE[0]}" && "${script##*/}" != cpu-cap-test.bash ]] || continue
	runs="$(grep -c 'docker run' "${script}" || true)"
	((runs)) || continue
	capped="$(grep -c -- '--cpus "${cpuJobs}" --cpu-shares 256 --env SHCLW_JOBS="${cpuJobs}"' "${script}" || true)"
	niced="$(grep -c '"${tag}" nice -n 19 bash ' "${script}" || true)"
	[[ "${capped}" == "${runs}" && "${niced}" == "${runs}" ]] || fFail "${script##*/}: ${runs} docker run(s), ${capped} capped, ${niced} niced"
done
for script in c-in-container.bash lang-in-container.bash; do
	grep -qF 'jobs="${SHCLW_JOBS:-}"' "${here}/${script}" || fFail "${script} doesn't take its count from SHCLW_JOBS"
done

if ((fails)); then echo "dedkv: ${fails} failure(s)" >&2; exit 1; fi
echo "PASS dedkv CPU caps and low priority"
