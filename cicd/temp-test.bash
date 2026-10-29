#!/usr/bin/env bash

#  shellcheck disable=1091  ## 'Not following.' The include is found at run time.
#  shellcheck disable=2016  ## The greps look for literal text.
#  shellcheck disable=2154  ## runDir is set by the include.

##	Purpose: Test deean. Run folders and safe removal, per Temp files and Safe
##		removal in the shared CICD rules (2026102416400070). The guard refuses
##		what isn't ours, a run folder is named STAMP_NNN under a temp root with
##		its links resolved, the sweep takes only stale run folders, and no
##		recursive removal in cicd or the C Makefile goes around the guard.
##	Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
##	SPDX-License-Identifier: MIT

set -Eeuo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
root="${here%/*}"
fFail(){ echo "deean: $*" >&2; exit 1; }

## Outside any folder named tmp, so the own-root rule is what lets a removal through.
## macOS: TMPDIR sits behind the /var link and ends in a slash.
stand="${root}/cicd/artifacts/temp-test.$$"
mkdir -p "${stand}/real/T"
: > "${stand}/.temp-test-owned"
ln -s real "${stand}/var"
HOST_TEMP="${stand}/var/T/"
source "${here}/utility/include/safe-rm.bash"
fCleanup(){
	if [[ -f "${stand}/.temp-test-owned" ]]; then fSafeRm_Under "${root}/cicd/artifacts" "${stand}" >/dev/null || true; fi
	return 0
}
trap fCleanup EXIT
own="${stand}/real/T/shclwrap"
[[ "${HOST_TEMP}" == "${stand}/real/T" ]] || fFail "HOST_TEMP wasn't resolved: ${HOST_TEMP}"

## fRefused WHY CMD...: CMD fails with WHY and removes nothing.
fRefused(){
	local why="$1" err
	shift
	if err="$("$@" 2>&1 >/dev/null)"; then fFail "accepted: $*"; fi
	[[ "${err}" == *"${why}"* ]] || fFail "$* said '${err}', not '${why}'"
}

## Run folders.
fRunDir 20261025-120000 || fFail "fRunDir failed"
[[ "${runDir}" == "${own}/20261025-120000_001" ]] || fFail "first run folder: ${runDir}"
[[ "$(cat "${runDir}/run.pid")" == "$$" && -f "${runDir}/.shclw-cicd-owned" ]] || fFail "no run.pid or mark in ${runDir}"
mkdir "${own}/20261025-120000_002"
fRunDir 20261025-120000
[[ "${runDir}" == "${own}/20261025-120000_003" ]] || fFail "didn't count past a taken name: ${runDir}"

## The guard.
mkdir -p "${own}/x/sub" "${stand}/real/other/sub"
ln -s "${stand}/real/other" "${own}/link"
fRefused "bad path"            fSafeRm ""
fRefused "bad path"            fSafeRm "${own}/x" "${own}/x"
fRefused "bad path"            fSafeRm "${own}/x/../x"
fRefused "bad path"            fSafeRm "${own}//x"
fRefused "bad path"            fSafeRm "${own}/x"$'\n'
fRefused "not under a tmp"     fSafeRm "${own}"
fRefused "not under a tmp"     fSafeRm "${stand}/real/other"
fRefused "not under a tmp"     fSafeRm "/no-such-root-$$/tmp"
fRefused "not under a tmp"     fSafeRm "relative/tmp/x"
fRefused "a link"              fSafeRm "${own}/link"
fRefused "goes through a link" fSafeRm "${own}/link/sub"
fRefused "not under a tmp"     fSafeRm "${stand}/var/T/shclwrap/x"
fRefused "not there"           fSafeRm "${own}/nothing"
[[ -d "${stand}/real/other/sub" && -d "${own}/x/sub" ]] || fFail "a refused removal removed something"
[[ "$(fSafeRm "${own}/x/")" == "removed ${own}/x" && ! -e "${own}/x" ]] || fFail "a folder under the run root wasn't removed"
fRefused "not under"           fSafeRm_Under "${stand}/real/other" "${stand}/real/other"
fRefused "not under"           fSafeRm_Under "${stand}/real/other" "${stand}/real/T"
fRefused "not under"           fSafeRm_Under / "${stand}/real/other/sub"
fRefused "a link"              fSafeRm_Under "${own}" "${own}/link"
fRefused "goes through a link" fSafeRm_Under "${stand}/real" "${stand}/real/T/shclwrap/link/sub"
mkdir "${own}/y"
[[ "$(fSafeRm_Under "${stand}/var/T" "${stand}/var/T/shclwrap/y")" == "removed ${own}/y" ]] || fFail "fSafeRm_Under refused a parent behind a link"
fSafeRm_Under "${stand}/real/other" "${stand}/real/other/sub" >/dev/null || fFail "fSafeRm_Under refused a folder inside its parent"
[[ ! -e "${stand}/real/other/sub" ]] || fFail "fSafeRm_Under left the folder"
[[ "$(bash "${here}/utility/include/safe-rm.bash" --under "${stand}/real" "${stand}/real/other")" == "removed ${stand}/real/other" ]] || fFail "the script form didn't remove"

## make clean, through the guard.
mkdir -p "${stand}/real/b/plain"
if make -s -C "${root}/source/c" OUT="${stand}/real/b" clean >/dev/null 2>&1; then fFail "make clean took a folder outside source/c"; fi
[[ -d "${stand}/real/b/plain" ]] || fFail "a refused make clean removed its folder"
make -s -C "${root}/source/c" OUT="${stand}/real/b" CLEAN_UNDER="${stand}/real" clean >/dev/null || fFail "make clean refused a folder under CLEAN_UNDER"
[[ ! -e "${stand}/real/b" ]] || fFail "make clean left its folder"
make -s -C "${root}/source/c" OUT="${stand}/real/b" CLEAN_UNDER="${stand}/real" clean >/dev/null || fFail "make clean failed with nothing to remove"

## The sweep: only marked run folders over 2 days old whose run.pid is gone.
sleep 0 & wait "$!"; dead="$!"
## fAge DAYS PATH. touch -d is GNU only.
fAge(){ python3 -c 'import os, sys, time; t = time.time() - float(sys.argv[1]) * 86400; os.utime(sys.argv[2], (t, t))' "$@"; }
fStale(){   ## fStale NAME PID DAYS [nomark]
	mkdir "${own}/$1"
	if [[ -n "$2" ]]; then printf '%s\n' "$2" > "${own}/$1/run.pid"; fAge "$3" "${own}/$1/run.pid"; fi
	if [[ "${4:-}" != nomark ]]; then : > "${own}/$1/.shclw-cicd-owned"; fi
}
fStale 20261020-000000_001 "${dead}" 3
fStale 20261020-000000_002 "$$"      3
fStale 20261020-000000_003 "${dead}" 1
fStale 20261020-000000_004 ""        0
fAge 3 "${own}/20261020-000000_004"
fStale 20261020-000000_005 "${dead}" 3 nomark
fStale handmade            "${dead}" 3
out="$(fRunDir_Sweep)"
[[ ! -e "${own}/20261020-000000_001" ]] || fFail "the stale run folder was kept"
for keep in 20261020-000000_002 20261020-000000_003 20261020-000000_004 20261020-000000_005 handmade 20261025-120000_001; do
	[[ -d "${own}/${keep}" ]] || fFail "the sweep removed ${keep}"
done
[[ "${out}" == "Removed 1 stale run folder(s) from ${own}" ]] || fFail "sweep said: ${out}"

## Wiring: the run's exports, the containers' temp folder, and no removal around the guard.
grep -qxF 'export HOST_TEMP TMPDIR="${work}" TEMP="${work}" TMP="${work}"' "${here}/cicd.bash" || fFail "cicd.bash doesn't point TMPDIR, TEMP and TMP at the run folder"
for script in c-checks.bash lang-checks.bash; do
	grep -qF -- '--env TMPDIR=/work --env TEMP=/work --env TMP=/work' "${here}/${script}" || fFail "${script}: the container's temp folder isn't the run's"
done
## Image builds clean their own layers, and the guard has the one real removal.
bad="$(git -C "${root}" grep -nE 'rm[[:space:]]+-[a-zA-Z]*[rR]|shutil[.]rmtree|Remove-Item[^#]*-Recurse' -- cicd source/c/Makefile \
	':!cicd/*.Dockerfile' ':!cicd/utility/include/safe-rm.bash' ':!cicd/cache' ':!cicd/artifacts' \
	| grep -vE '^cicd/fuzz[.]py:[0-9]+:[[:space:]]*shutil[.]rmtree[(]path[)]$|^cicd/remote-windows[.]ps1:[0-9]+:[[:space:]]*Remove-Item -LiteralPath [$]p -[R]ecurse -Force$' || true)"
[[ -z "${bad}" ]] || fFail "a recursive removal goes around the guard: ${bad}"
echo "PASS deean run folders and safe removal"


##	History:
##		- 2026-10-25 JC: Created.
