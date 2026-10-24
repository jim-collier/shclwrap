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


##	History:
##		- 2026-10-24 JC: Created.
