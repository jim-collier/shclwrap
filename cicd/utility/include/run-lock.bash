#!/usr/bin/env bash

##	Purpose: One cicd run per tree at a time. Runs share cicd/artifacts/, cicd/cache/,
##		and the checkout itself, so a second run waits for the first
##		(backlog 2026102323244600). Source this and call:
##			fRunLock ROOT WHAT
##		A script the locked run starts, such as c-checks.bash under cicd.bash, gets
##		through without waiting, by SHCLW_CICD_LOCK. The lock goes when the run
##		exits, killed or not.
##	Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT

fRunLock(){   ## fRunLock ROOT WHAT
	local lock="$1/cicd/artifacts/run.lock" what="$2" lockFd
	if [[ "${SHCLW_CICD_LOCK:-}" == "${lock}" ]]; then return 0; fi
	if ! command -v flock >/dev/null; then
		echo "WARNING: no flock here, so another cicd run in this tree can collide with this one" >&2
		return 0
	fi
	mkdir -p "${lock%/*}"
	exec {lockFd}>>"${lock}"
	if ! flock --nonblock "${lockFd}"; then
		echo "Waiting for the other cicd run in this tree to finish ($(cat "${lock}" 2>/dev/null || true))" >&2
		flock "${lockFd}"
	fi
	printf '%s, pid %s, since %s\n' "${what}" "$$" "$(date '+%Y-%m-%d %H:%M:%S')" > "${lock}"
	export SHCLW_CICD_LOCK="${lock}"
	return 0
}

##	Opens a new run_<stamp>.log in DIR on fd FD_VAR, sets STAMP_VAR to its stamp,
##	and rotates the logs. The log starts before the run lock, so two runs started
##	in one second would share a name; the second waits for the next second
##	instead (2026102409544336). The rotation renames logs, so it holds the same
##	short lock as the name check. Needs gfs-rotate.bash.
fRunLog(){   ## fRunLog DIR STAMP_VAR FD_VAR
	local dir="$1"; local -n stamp_q4w="$2" fd_q4w="$3"
	local nameLock="" try taken f hadNoclobber=0 opened=0
	mkdir -p "${dir}"
	if command -v flock >/dev/null; then exec {nameLock}>>"${dir}/.name.lock"; flock "${nameLock}"; fi
	[[ $- == *C* ]] && hadNoclobber=1
	for ((try = 0; try < 50; try++)); do
		printf -v stamp_q4w '%(%Y%m%d-%H%M%S)T' -1
		taken=0
		for f in "${dir}/run_${stamp_q4w}.log" "${dir}/run_${stamp_q4w}_"*.log; do [[ -e "${f}" ]] && taken=1; done
		if ((! taken)); then
			set -C
			# shellcheck disable=SC2034  # fd_q4w is the caller's, through the nameref
			if { exec {fd_q4w}>"${dir}/run_${stamp_q4w}.log"; } 2>/dev/null; then opened=1; fi
			((hadNoclobber)) || set +C
			((opened)) && break
		fi
		sleep 0.1
	done
	if ((opened)); then gfs_rotate "${dir}" run log >/dev/null 2>&1 || true; fi
	if [[ -n "${nameLock}" ]]; then exec {nameLock}>&-; fi
	((opened)) || { echo "no free run log name in ${dir}" >&2; return 1; }
	return 0
}


##	History:
##		- 2026-10-24 JC: Created.
##		- 2026-10-24 JC: fRunLog, so two runs started in one second get their own logs.
