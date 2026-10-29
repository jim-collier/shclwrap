#!/usr/bin/env bash
#  shellcheck disable=1091  ## 'Not following.' The pins files are found at run time.
#  shellcheck disable=2016  ## 'Expressions don't expand in single quotes.' That's PowerShell for the box.

##	- Purpose: Run the Go, Rust, Python and C tests and the shared cases on the
##	  other test boxes, natively. Ships the tracked tree and the pinned shcl
##	  caches as one tarball, then holds each box through the shared host lock's
##	  wrap for the whole time it's used, which also puts a VM back as found.
##	- Syntax: remote-test.bash WORK [target...]
##	  WORK is the run's scratch folder. Targets: windows. Default: all of them.
##	- A box that doesn't answer, or can't be held, is a SKIP line, never a
##	  failure. Only a test that ran and failed fails the run.
##	- The wrap helper is SHCLW_VM_WRAP, else the *_vm-state.bash in the synced
##	  util folder. Without one every target is a SKIP.
##	- SHCLW_REMOTE_WAIT is how long to wait in line for a box, default 900s.
##	- SHCLW_KEEP_TEMP=1 keeps the box's work folder, and says where it is.
##	- Each box line keeps its test's own ID, with " on <host>" added.

##	History: At bottom of script.

##	Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT

set -Eeuo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${here}/.." && pwd)"
meFile="${here}/remote-test.bash"

## target | test ID | what | ssh destinations, first that answers
declare -ra targetTable=(
	"windows | dejex | Windows tests | collierjr@b29w"
)
declare -ra sshOpts=(-o BatchMode=yes -o ConnectTimeout=15 -o ServerAliveInterval=30 -o ServerAliveCountMax=4)

fWarn(){ printf 'WARNING: %s\n' "$*" >&2; }
fDie(){  printf 'remote-test: %s\n' "$*" >&2; exit 1; }
fTrim(){ local -n s_r7tq="$1"; s_r7tq="${s_r7tq#"${s_r7tq%%[![:space:]]*}"}"; s_r7tq="${s_r7tq%"${s_r7tq##*[![:space:]]}"}"; }
## Fills target, tid, what and dests in the caller from one table row.
fParseRow(){
	local -a f; IFS='|' read -r -a f <<< "$1"
	target="${f[0]}"; tid="${f[1]}"; what="${f[2]}"; dests="${f[3]}"
	fTrim target; fTrim tid; fTrim what; fTrim dests
}

## The tracked tree as it is on disk, plus the pinned shcl caches, so a box tests
## what this run tests, uncommitted edits included.
fBundle(){
	local -r out="$1"
	local f
	source "${root}/source/c/shcl-pins.env"
	source "${root}/source/python/shcl-pins.env"
	{	while IFS= read -r -d '' f; do [[ ! -e "${root}/${f}" ]] || printf '%s\0' "${f}"; done < <(git -C "${root}" ls-files -z)
		printf '%s\0' "cicd/cache/shcl-c/${SHCL3_REV}/shcl.h" "cicd/cache/shcl-c/${SHCL2_REV}/shcl.h" "cicd/cache/shcl-py/${SHCL3_PY_REV}/shcl.py"
	} | tar -C "${root}" --null -T - -czf "${out}"
	printf '%s %s %s\n' "${SHCL3_REV}" "${SHCL2_REV}" "${SHCL3_PY_REV}" > "${out%/*}/pins"
}

## Runs inside the wrap, so the box is held and awake. Writes a status file the
## caller reads, since the wrap's own exit also covers putting a VM back.
fOnBox(){
	local -r runDir="$1" target="$2" dest="$3"
	local -r statusFile="${runDir}/${target}.status"
	local -r mkWork='$r = if ($env:HOST_TEMP) { $env:HOST_TEMP } else { $env:TEMP }
		$r = Join-Path $r shclwrap; $null = New-Item -ItemType Directory -Force $r
		$s = Get-Date -Format yyyyMMdd-HHmmss
		for ($n = 1; $n -le 999; $n++) {
			$p = "{0}\{1}_{2:d3}" -f $r, $s, $n
			try { $null = New-Item -ItemType Directory $p -ErrorAction Stop; $p; break } catch { }
		}'
	local work rc=0 s3 s2 p3
	echo "started" > "${statusFile}"
	if ! ssh "${sshOpts[@]}" "${dest}" exit 0 < /dev/null &> /dev/null; then echo "noanswer" > "${statusFile}"; return 0; fi
	## Made once a box answers, and once for every box.
	if [[ ! -s "${runDir}/tree.tgz" ]] && ! fBundle "${runDir}/tree.tgz"; then
		echo "fail" > "${statusFile}"; fWarn "${target}: couldn't pack the tree and the pinned shcl caches"; return 1
	fi
	## ssh there opens cmd, so the setup goes as encoded PowerShell.
	work="$(ssh "${sshOpts[@]}" "${dest}" pwsh -NoProfile -EncodedCommand "$(iconv -f utf-8 -t utf-16le <<< "${mkWork}" | base64 -w0)" < /dev/null 2> /dev/null | tr -d '\r')" \
		|| { echo "fail" > "${statusFile}"; fWarn "${target}: couldn't make a work folder on ${dest}"; return 1; }
	work="${work##*$'\n'}"
	[[ "${work}" =~ ^[A-Za-z]:\\[A-Za-z0-9._~\\-]+\\shclwrap\\[0-9]{8}-[0-9]{6}_[0-9]{3}$ ]] \
		|| { echo "fail" > "${statusFile}"; fWarn "${target}: odd work folder '${work}'"; return 1; }
	echo "${target}: ${dest}:${work}"
	read -r s3 s2 p3 < "${runDir}/pins"
	scp -q "${sshOpts[@]}" "${runDir}/tree.tgz" "${here}/remote-windows.ps1" "${dest}:${work//\\//}/" 2> /dev/null || rc=1
	if ((rc == 0)); then
		## Each line from the box keeps its ID and says where it ran. The ** lines are ssh's own notice.
		timeout 2400 ssh "${sshOpts[@]}" "${dest}" \
			pwsh -NoProfile -ExecutionPolicy Bypass -File "${work}\\remote-windows.ps1" -Work "${work}" -Shcl3 "${s3}" -Shcl2 "${s2}" -Py3 "${p3}" -KeepTemp "${SHCLW_KEEP_TEMP:-0}" \
			< /dev/null 2> >(grep -v '^\*\*' >&2) | sed -u -E "s/\r$//; s/^(PASS|FAIL|SKIP) (.*)$/\1 \2 on ${dest#*@}/" || rc=$?
	fi
	if ((rc == 0)); then echo "pass" > "${statusFile}"; else echo "fail" > "${statusFile}"; fi
	return "${rc}"
}

fMain(){
	if [[ "${1:-}" == --on-box ]]; then shift; fOnBox "$@"; return; fi
	local -r work="${1:?usage: remote-test.bash WORK [target...]}"; shift
	local -a want=("$@") rows=()
	local row target tid what dests dest host status vmWrap="${SHCLW_VM_WRAP:-}" why=""
	local -i failed=0 wrapRc
	for row in "${targetTable[@]}"; do
		fParseRow "${row}"
		if ((${#want[@]})) && [[ " ${want[*]} " != *" ${target} "* ]]; then continue; fi
		rows+=("${row}")
	done
	((${#rows[@]})) || fDie "no such target: ${want[*]}"
	if [[ -z "${vmWrap}" ]]; then
		local -a found=("${HOME}"/synced/0-0/common/exec/util/linux/bash/*_vm-state.bash)
		[[ ! -x "${found[0]}" ]] || vmWrap="${found[0]}"
	fi
	[[ -n "${vmWrap}" && -x "${vmWrap}" ]] || why="no host lock wrap helper here"
	command -v ssh > /dev/null || why="no ssh here"
	if [[ -n "${why}" ]]; then
		for row in "${rows[@]}"; do fParseRow "${row}"; echo "SKIP ${tid} ${what}: ${why}"; done
		return 0
	fi

	local -r runDir="${work}/remote"
	mkdir -p "${runDir}"
	for row in "${rows[@]}"; do
		fParseRow "${row}"
		status="noanswer"
		for dest in ${dests}; do
			host="${dest#*@}"
			rm -f -- "${runDir}/${target}.status"
			wrapRc=0
			"${vmWrap}" wrap "${host}" --wait "${SHCLW_REMOTE_WAIT:-900}" -- bash "${meFile}" --on-box "${runDir}" "${target}" "${dest}" || wrapRc=$?
			status="$(cat "${runDir}/${target}.status" 2> /dev/null || echo unheld)"
			case "${status}" in
				pass) echo "PASS ${tid} ${what} on ${host}" ;;
				fail) echo "FAIL ${tid} ${what} on ${host}"; failed+=1 ;;
				*)    fWarn "${target}: couldn't hold or reach ${host} (${status}, exit ${wrapRc})"; status="noanswer"; continue ;;
			esac
			## The status is the tests' result, so a non-zero wrap after a pass is the put-back.
			if [[ "${status}" == pass ]] && ((wrapRc)); then fWarn "${target}: ${host} may not be back as it was found"; fi
			break
		done
		[[ "${status}" != noanswer ]] || echo "SKIP ${tid} ${what}: no answer from ${dests//[^ ]*@/}"
	done
	((failed == 0))
}

fMain "$@"


##	History:
##		- 2026-10-28 JC: Created. Windows on b29w, through the host lock's wrap. Modeled on gitniblet-floss's remote-test.bash.
