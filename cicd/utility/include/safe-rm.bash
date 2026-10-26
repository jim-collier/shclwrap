#!/usr/bin/env bash

#  shellcheck disable=2034  ## runDir is for the caller.

##	Purpose: Temp folders and safe removal, per Temp files and Safe removal in
##		the shared CICD rules (backlog 2026102416400070). Source this and call:
##			fSafeRm PATH            rm -rf, only for a path under a folder named
##			                        tmp, or under ${HOST_TEMP}/shclwrap/
##			fSafeRm_Under DIR PATH  rm -rf, only for a path inside DIR
##			fRunDir STAMP           makes the run's folder, ${HOST_TEMP}/shclwrap/STAMP_NNN,
##			                        with a plain mkdir, writes run.pid and the
##			                        ownership mark, and sets runDir
##			fRunDir_Sweep           removes the project's run folders older than
##			                        2 days whose run.pid names no live process
##		Both removals refuse '..', '//', a newline, a link, a path through a
##		link, or one that isn't there, and stay on one filesystem.
##		Sourcing it sets HOST_TEMP, with links resolved: HOST_TEMP, else TEMP on
##		Windows, else ${TMPDIR:-/tmp}. macOS's temp folder sits behind /var.
##		Run as a script for make clean: safe-rm.bash [--under DIR] PATH
##	Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT

projectName=shclwrap
## A folder's real path. Portable, since macOS's realpath has no -e.
fRealDir(){ (cd -P -- "$1" 2>/dev/null && pwd -P); }

HOST_TEMP="${HOST_TEMP:-}"
if [[ -z "${HOST_TEMP}" && ( "${OSTYPE:-}" == msys* || "${OSTYPE:-}" == cygwin* ) ]]; then HOST_TEMP="${TEMP:-}"; fi
HOST_TEMP="${HOST_TEMP:-${TMPDIR:-/tmp}}"
HOST_TEMP="$(fRealDir "${HOST_TEMP}" || printf '%s' "${HOST_TEMP%/}")"

## BSD rm calls --one-file-system -x.
if rm --version >/dev/null 2>&1; then _rmOneFs=--one-file-system; else _rmOneFs=-x; fi

_fSafeRm_Bad(){ [[ -z "$1" || "$1" == *..* || "$1" == *//* || "$1" == *$'\n'* ]]; }

## The checks both removals share, then the removal.
_fSafeRm_Do(){
	local p="$1" real=""
	if [[ -L "${p}" ]]; then printf 'safe-rm: refused, a link: %s\n' "${p}" >&2; return 1; fi
	if [[ -d "${p}" ]]; then real="$(fRealDir "${p}")"
	elif [[ -e "${p}" ]]; then real="$(fRealDir "${p%/*}")/${p##*/}"
	fi
	[[ -n "${real}" ]] || { printf 'safe-rm: refused, not there: %s\n' "${p}" >&2; return 1; }
	if [[ "${real}" != "${p}" ]]; then printf 'safe-rm: refused, goes through a link: %s\n' "${p}" >&2; return 1; fi
	rm -rf "${_rmOneFs}" -- "${p}" && printf 'removed %s\n' "${p}"
}

fSafeRm(){
	local p="${1:-}" own="${HOST_TEMP:?}/${projectName:?}"
	if [[ "${#}" -ne 1 ]] || _fSafeRm_Bad "${p}"; then
		printf 'safe-rm: refused, bad path: "%s"\n' "${p}" >&2; return 1
	fi
	p="${p%/}"
	if ! [[ "${p}" =~ ^/(.+/)?tmp/[^/]+(/[^/]+)*$ || "${p}" == "${own}"/?* ]]; then
		printf 'safe-rm: refused, not under a tmp folder: %s\n' "${p}" >&2; return 1
	fi
	_fSafeRm_Do "${p}"
}

## For what lives outside a temp root, such as cicd/artifacts/fuzz/ or a build folder.
## DIR may sit behind a link, as a checkout can, but nothing below it may.
fSafeRm_Under(){   ## fSafeRm_Under DIR PATH
	local dir="${1:-}" p="${2:-}" realDir=""
	if [[ "${#}" -ne 2 ]] || _fSafeRm_Bad "${dir}" || _fSafeRm_Bad "${p}"; then
		printf 'safe-rm: refused, bad path: "%s" under "%s"\n' "${p}" "${dir}" >&2; return 1
	fi
	dir="${dir%/}"; p="${p%/}"
	if ! [[ "${dir}" =~ ^/[^/]+ && "${p}" == "${dir}"/?* ]]; then
		printf 'safe-rm: refused, not under %s: %s\n' "${dir:-/}" "${p}" >&2; return 1
	fi
	realDir="$(fRealDir "${dir}")" || { printf 'safe-rm: refused, not there: %s\n' "${dir}" >&2; return 1; }
	_fSafeRm_Do "${realDir}/${p#"${dir}"/}"
}

fRunDir(){   ## fRunDir STAMP
	local own="${HOST_TEMP}/${projectName}" n
	mkdir -p -- "${own}" || return 1
	for ((n = 1; n < 1000; n++)); do
		printf -v runDir '%s/%s_%03d' "${own}" "$1" "${n}"
		if mkdir -- "${runDir}" 2>/dev/null; then
			printf '%s\n' "$$" > "${runDir}/run.pid"
			: > "${runDir}/.shclw-cicd-owned"
			return 0
		fi
	done
	echo "no free run folder name for $1 under ${own}" >&2
	return 1
}

## Only folders named and marked as runs. A folder made by hand has no run.pid, so it stays.
fRunDir_Sweep(){
	local own="${HOST_TEMP}/${projectName}" pidFile dir pid swept=0
	[[ -d "${own}" ]] || return 0
	while IFS= read -r pidFile; do
		dir="${pidFile%/run.pid}"
		[[ "${dir##*/}" =~ ^[0-9]{8}-[0-9]{6}_[0-9]{3}$ && -f "${dir}/.shclw-cicd-owned" ]] || continue
		pid=""; read -r pid < "${pidFile}" || true
		[[ "${pid}" =~ ^[0-9]+$ ]] || continue
		## ps rather than kill -0, which fails on another user's live process.
		if ps -p "${pid}" >/dev/null 2>&1; then continue; fi
		if fSafeRm "${dir}" >/dev/null; then swept=$((swept + 1)); fi
	done < <(find "${own}" -mindepth 2 -maxdepth 2 -name run.pid -mmin +2880 2>/dev/null)
	if ((swept)); then echo "Removed ${swept} stale run folder(s) from ${own}"; fi
	return 0
}

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
	set -Eeuo pipefail
	if [[ "${1:-}" == --under ]]; then fSafeRm_Under "${@:2}"; else fSafeRm "$@"; fi
fi
