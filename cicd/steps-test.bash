#!/usr/bin/env bash

#  shellcheck disable=2016  ## The stand-in runs are single-quoted on purpose.

##	Purpose: Test dedm7. cicd's step lines: a 79-wide rule, "Step N of M" with a
##		local time to the hundredth, Starting then PASS or FAIL, steps turned off
##		left out of the count and listed with their flag, and single blank lines
##		(2026102416400040).
##	Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
##	SPDX-License-Identifier: MIT

set -Eeuo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lib="${here}/utility/include/steps.bash"
fails=0
fFail(){ echo "dedm7: $*" >&2; fails=$((fails + 1)); }
[[ -f "${lib}" ]] || { echo "dedm7: no ${lib}" >&2; exit 1; }

rule=""; for ((i = 0; i < 79; i++)); do rule+="•"; done
ts='[0-9]{8}-[0-9]{6}\.[0-9]{2}'

## fRun SCRIPT: runs a stand-in with the include and cicd's ERR trap, into out and rc.
## The status rides on the end, since $() would drop the blank line there.
fRun(){
	out="$(bash -s "${lib}" 2>&1 <<-EOF; printf '|%s' "$?"
		set -Eeuo pipefail
		source "\$1"
		trap 'fStep_Abort "\$?" "\${LINENO}" "\${BASH_COMMAND}"' ERR
		fStep_Plan A B C D
		fStep_Off B "--no-b"
		fStep_Off D "--no-d"
		$1
		EOF
	)"
	rc="${out##*|}"; out="${out%|*}"
	return 0
}

## fBlanks: one blank line before each rule, never two in a row, and one at the end.
fBlanks(){
	local -a lines=()
	local j
	mapfile -t lines < <(printf '%s' "${out}")
	[[ -z "${lines[-1]}" ]] || fFail "$1: no blank line at the end"
	for ((j = 1; j < ${#lines[@]}; j++)); do
		if [[ -z "${lines[j]}" && -z "${lines[j - 1]}" ]]; then fFail "$1: two blank lines at line $((j + 1))"; fi
		if [[ "${lines[j]}" == "${rule}" && -n "${lines[j - 1]}" ]]; then fFail "$1: no blank line before the rule at line $((j + 1))"; fi
	done
	return 0
}

## A passing run, with 2 of 4 steps turned off.
fRun 'fSection "first"; fStep_Summary
	fStep_Start A; echo "a out"; fStep_Pass
	fStep_Start C; fStep_Pass
	fStep_AllPassed; fSection "done"; fEcho_Clean'
((rc == 0)) || fFail "passing run exited ${rc}: ${out}"
mapfile -t lines < <(printf '%s' "${out}")
want=("" "${rule}" "[ first ]" "Steps ........: 2: A, C" "Turned off ...: B (--no-b)" "                D (--no-d)"
	"" "${rule}" "@Step 1 of 2; Starting A" "a out" "@Step 1 of 2; PASS A"
	"" "${rule}" "@Step 2 of 2; Starting C" "@Step 2 of 2; PASS C"
	"" "${rule}" "[ done ]" "")
((${#lines[@]} == ${#want[@]})) || fFail "passing run gave ${#lines[@]} lines, want ${#want[@]}: ${out}"
for ((i = 0; i < ${#want[@]}; i++)); do
	if [[ "${want[i]}" == @* ]]; then
		[[ "${lines[i]:-}" =~ ^\[\ ${ts}:\ ${want[i]#@}\ \]$ ]] || fFail "line $((i + 1)) is '${lines[i]:-}', want '[ <time>: ${want[i]#@} ]'"
	else
		[[ "${lines[i]:-}" == "${want[i]}" ]] || fFail "line $((i + 1)) is '${lines[i]:-}', want '${want[i]}'"
	fi
done
fBlanks "passing run"
## The time is local, to the hundredth.
stamp="${lines[8]#\[ }"; stamp="${stamp%%:*}"
today="$(date +%Y%m%d)"
[[ "${stamp}" == "${today}"-* || "${stamp}" == "$(date -d yesterday +%Y%m%d 2>/dev/null || echo none)"-* ]] || fFail "step time '${stamp}' isn't today's local date ${today}"

## None turned off.
fRun 'fStep_Plan A B; fStep_Summary'
[[ "${out}" == "Steps ........: 2: A, B"$'\n'"Turned off ...: none"$'\n' ]] || fFail "with none off: ${out}"

## A failing command closes the open step with FAIL, once, from a subshell too,
## where cicd's fDie also calls fStep_Fail.
for cmd in 'false' '(cd / && false)' '(fStep_Fail; exit 1)'; do
	fRun "fStep_Start A; fStep_Pass; fStep_Start C; ${cmd}; fStep_Pass"
	((rc != 0)) || fFail "'${cmd}': the run passed"
	n="$(grep -cE "^\[ ${ts}: Step 2 of 2; FAIL C \]$" <<<"${out}" || true)"
	((n == 1)) || fFail "'${cmd}': ${n} FAIL lines, want 1: ${out}"
	if grep -q 'PASS C' <<<"${out}"; then fFail "'${cmd}': C passed: ${out}"; fi
	fBlanks "'${cmd}'"
done

## No FAIL line when no step is open.
fRun 'fStep_Start A; fStep_Pass; false'
if grep -q 'FAIL' <<<"${out}"; then fFail "a failure outside a step printed a FAIL line: ${out}"; fi

## A step that's turned off, or not in the plan, can't start.
fRun 'fStep_Start B'
((rc != 0)) || fFail "a step turned off started: ${out}"
fRun 'fStep_Start E'
((rc != 0)) || fFail "a step not in the plan started: ${out}"

## A step that's on but never ran fails the end check.
fRun 'fStep_Start A; fStep_Pass; fStep_AllPassed'
((rc != 0)) || fFail "1 of 2 steps run passed the end check: ${out}"

## cicd.bash names the flag that turned each step off.
## fOff "FLAGS" "WANT"...: each WANT is a step and its reason, one per off step, in order.
fOff(){
	local flags="$1" plan got=""
	shift
	# shellcheck disable=2086  ## flags is split into words on purpose.
	plan="$(env -u SHCLW_FUZZ_SECONDS -u SHCLW_GOFUZZ_SECONDS bash "${here}/cicd.bash" --dry-run ${flags})"
	got="$(sed -n '/^Turned off \.\.\.: /,$p' <<<"${plan}" | sed -E 's/^(Turned off \.\.\.:)? +//; /^$/d' | paste -sd '|')"
	[[ "${got}" == "$(IFS='|'; echo "$*")" ]] || fFail "'${flags}': turned off '${got}', want '$(IFS='|'; echo "$*")'"
	return 0
}
fOff ""                        "none"
fOff "--ci"                    "C leak checks (--ci without --leakcheck)" "Fuzz (--ci)"
fOff "--quick"                 "Remote sync (--quick)" "C leak checks (--quick without --leakcheck)"
fOff "--ci --quick"            "Remote sync (--quick)" "C leak checks (--ci --quick without --leakcheck)" "Fuzz (--ci)"
fOff "--ci --leakcheck"        "Fuzz (--ci)"
fOff "--leakcheck --no-leakcheck --no-sync --no-fuzz" "Remote sync (--no-sync)" "C leak checks (--no-leakcheck)" "Fuzz (--no-fuzz)"
fOff "--quick --no-sync"       "Remote sync (--no-sync)" "C leak checks (--quick without --leakcheck)"
fOff "--quiet --ci --no-fuzz"  "C leak checks (--ci without --leakcheck)" "Fuzz (--no-fuzz)"

if ((fails)); then echo "dedm7: ${fails} check(s) failed" >&2; exit 1; fi
echo "OK: step lines (dedm7)"


##	History:
##		- 2026-10-25 JC: Created.
