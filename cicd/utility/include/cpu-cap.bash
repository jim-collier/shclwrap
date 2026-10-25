#!/usr/bin/env bash

#  shellcheck disable=2034  ## cpuBusy and friends are for the caller.

##	Purpose: CPU and priority caps for a cicd run, per Machine use in the shared
##		CICD rules (backlog 2026102416400030). Source this and call:
##			fLowPriority    nice 19, and idle IO where ionice exists, for this
##			                shell and everything it starts
##			fCpuCap [hard]  sets cpuJobs, cpuCount, cpuLoad and cpuBusy, and
##			                exports the count to the tools that read it from
##			                the environment
##		cpuJobs is half the CPUs, or a quarter when the 1-minute load is over
##		half the CPU count, or for a hard step (leak checks, sanitized builds,
##		soak, fuzz). Never under 1. Call fCpuCap again before each step.
##	Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT

fLowPriority(){
	renice -n 19 -p "$$" >/dev/null 2>&1 || true
	if command -v ionice >/dev/null; then ionice -c 3 -p "$$" >/dev/null 2>&1 || true; fi
	return 0
}

## Linux, then macOS and the BSDs.
fCpuCount(){
	local n=""
	n="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || true)"
	[[ "${n}" =~ ^[1-9][0-9]*$ ]] || n=2
	echo "${n}"
}
fLoad1(){
	local a=""
	if [[ -r /proc/loadavg ]]; then read -r a _ </proc/loadavg
	else read -r _ a _ < <(sysctl -n vm.loadavg 2>/dev/null || true)   ## "{ 1.23 1.45 1.67 }"
	fi
	echo "${a:-0}"
}

## fCpuJobs COUNT LOAD1 [hard]: prints the job count. A load that isn't a plain number counts as idle.
fCpuJobs(){
	local count="$1" load="$2" hard="${3:-}" whole frac centi=0 busy=0 jobs
	[[ "${count}" =~ ^[1-9][0-9]*$ ]] || count=2
	if [[ "${load}" =~ ^([0-9]+)(\.([0-9]*))?$ ]]; then
		whole="${BASH_REMATCH[1]}"; frac="${BASH_REMATCH[3]}00"
		centi=$((10#${whole} * 100 + 10#${frac:0:2}))
	fi
	if ((centi * 2 > count * 100)); then busy=1; fi
	if [[ "${hard}" == hard ]] || ((busy)); then jobs=$((count / 4)); else jobs=$((count / 2)); fi
	((jobs >= 1)) || jobs=1
	echo "${jobs} ${busy}"
}

fCpuCap(){   ## fCpuCap [hard]
	local out
	cpuCount="$(fCpuCount)"
	cpuLoad="$(fLoad1)"
	out="$(fCpuJobs "${cpuCount}" "${cpuLoad}" "${1:-}")"
	cpuJobs="${out% *}"; cpuBusy="${out#* }"
	## go vet, staticcheck, the Go tests and fuzzer; ruff; Rust's test threads; the C container.
	export GOMAXPROCS="${cpuJobs}" RAYON_NUM_THREADS="${cpuJobs}" RUST_TEST_THREADS="${cpuJobs}" CARGO_BUILD_JOBS="${cpuJobs}" SHCLW_JOBS="${cpuJobs}"
	return 0
}


##	History:
##		- 2026-10-25 JC: Created.
