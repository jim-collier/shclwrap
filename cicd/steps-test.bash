#!/usr/bin/env bash

#  shellcheck disable=2016  ## The stand-in runs are single-quoted on purpose.

##	Purpose: Test dedm7. cicd's step lines: a 79-wide rule, "Step N of M" with a
##		local time to the hundredth, Starting then PASS or FAIL, steps turned off
##		left out of the count and listed with their flag, and single blank lines
##		(2026102416400040). Test lines: PASS, FAIL or SKIP, the ID and the name,
##		FAIL once however the test fails, and one name per ID across cicd
##		(2026102507142900).
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
fOff "--ci"                    "Remote sync (--merge-gate)"
fOff "--quick"                 "Remote sync (--quick)" "GitHub ci: start (--quick)" "C leak checks (--quick without --leakcheck)" "Profile (--quick)" "Other hosts (--quick)" "GitHub ci: result (--quick)"
fOff "--ci --quick"            "Remote sync (--quick)" "GitHub ci: start (--quick)" "C leak checks (--quick without --leakcheck)" "Profile (--quick)" "Other hosts (--quick)" "GitHub ci: result (--quick)"
fOff "--ci --leakcheck"        "Remote sync (--merge-gate)"
fOff "--leakcheck --no-leakcheck --no-sync --no-fuzz" "Remote sync (--no-sync)" "C leak checks (--no-leakcheck)" "Fuzz (--no-fuzz)"
fOff "--quick --no-sync"       "Remote sync (--no-sync)" "GitHub ci: start (--quick)" "C leak checks (--quick without --leakcheck)" "Profile (--quick)" "Other hosts (--quick)" "GitHub ci: result (--quick)"
fOff "--quiet --ci --no-fuzz"  "Remote sync (--merge-gate)" "Fuzz (--no-fuzz)"
fOff "--no-format --no-lint --no-tests" "Format (--no-format)" "Lint (--no-lint)" "Tests (--no-tests)"
fOff "--backlog-run-quick"     "Remote sync (--backlog-run-quick)" "GitHub ci: start (--backlog-run-quick)" "C leak checks (--backlog-run-quick without --leakcheck)" "Fuzz (--backlog-run-quick)" "Profile (--backlog-run-quick)" "Other hosts (--backlog-run-quick)" "GitHub ci: result (--backlog-run-quick)"
fOff "--backlog-run-full"      "Remote sync (--backlog-run-full)"
fOff "--no-container"          "C leak checks (--no-container)"
fOff "--no-remote"             "GitHub ci: start (--no-remote)" "Other hosts (--no-remote)" "GitHub ci: result (--no-remote)"

## Test lines: "PASS|FAIL|SKIP ID NAME", one per test, FAIL once whichever way it fails.
fRun 'fTest abcde "a name" true; fTest -q abcdf "quiet one" echo noise; fTest_Skip abcdg "skipped one" "no box"'
((rc == 0)) || fFail "passing tests exited ${rc}: ${out}"
[[ "${out}" == $'PASS abcde a name\nPASS abcdf quiet one\nSKIP abcdg skipped one, no box\n' ]] || fFail "passing tests: ${out}"
fRun 'fTest abcde "a name" false; echo after'
((rc != 0)) || fFail "a failing fTest passed"
[[ "$(grep -c '^FAIL abcde a name$' <<<"${out}")" == 1 ]] || fFail "a failing fTest: want 1 FAIL line: ${out}"
if grep -qE '^PASS|^after' <<<"${out}"; then fFail "a failing fTest went on: ${out}"; fi
## Under an if the ERR trap doesn't fire, so the FAIL is fTest's own.
fRun 'if ! fTest abcde "a name" false; then echo handled; fi'
[[ "${rc}" == 0 && "${out}" == $'FAIL abcde a name\nhandled\n' ]] || fFail "a failing fTest under if: rc ${rc}: ${out}"
fRun 'fNope(){ return 3; }; fTest abcde "a name" fNope'
[[ "${rc}" == 3 && "${out}" == *"FAIL abcde a name"* ]] || fFail "a function returning 3: rc ${rc}: ${out}"
## For --list: only the ID and name, and the command never runs. Then back to running.
fRun 'fTest_ListOnly 1; fTest -q abcde "a name" echo ran; fTest abcdf "two" false; fTest_ListOnly 0; fTest abcdg "three" true'
[[ "${rc}" == 0 && "${out}" == $'abcde a name\nabcdf two\nPASS abcdg three\n' ]] || fFail "fTest_ListOnly: rc ${rc}: ${out}"
fRun 'fStep_Each echo step'
[[ "${out}" == $'step A\nstep B\nstep C\nstep D\n' ]] || fFail "fStep_Each: ${out}"
## Inline, inside a step: the test's FAIL comes before the step's.
for cmd in 'false' '(cd / && false)'; do
	fRun "fStep_Start A; fTest_Start abcde 'an inline one'; ${cmd}; fTest_Pass; fStep_Pass"
	((rc != 0)) || fFail "inline '${cmd}': the run passed"
	[[ "$(grep -c '^FAIL abcde an inline one$' <<<"${out}")" == 1 ]] || fFail "inline '${cmd}': want 1 FAIL line: ${out}"
	order="$(grep -nE '^FAIL abcde|FAIL A \]$' <<<"${out}" | cut -d: -f1 | paste -sd ' ')"
	if ! [[ "${order}" =~ ^([0-9]+)\ ([0-9]+)$ ]] || ((BASH_REMATCH[1] > BASH_REMATCH[2])); then
		fFail "inline '${cmd}': the test's FAIL isn't before the step's: ${out}"
	fi
done
fRun 'fTest_Pass'
((rc != 0)) || fFail "fTest_Pass with no test open passed"
## The container's form: no ERR trap, an exit from inside the probe.
out="$(bash -s "${lib}" 2>&1 <<-'EOF'; printf '|%s' "$?"
	set -Eeuo pipefail
	source "$1"
	trap fTest_Fail EXIT
	fTest_Start abcde "one"; fTest_Pass
	fTest_Start abcdf "two"; exit 1
	EOF
)"
[[ "${out}" == $'PASS abcde one\nFAIL abcdf two\n|1' ]] || fFail "exit inside a test under an EXIT trap: ${out}"

## Every ID cicd prints is 5 letters of lowercase Crockford base 32, and keeps one name.
declare -A idName=()
while IFS=$'\t' read -r file id name; do
	[[ "${id}" =~ ^[0-9a-hjkmnp-tv-z]{5}$ ]] || fFail "${file}: '${id}' isn't a test ID"
	if [[ -n "${idName[${id}]+x}" && "${idName[${id}]}" != "${name}" ]]; then fFail "${file}: ${id} is '${name}' and '${idName[${id}]}'"; fi
	idName["${id}"]="${name}"
done < <(grep -oE '(fTest( -q)?|fTest_Start|fTest_Skip) [a-z0-9]{5} "[^"]+"|\[[a-z0-9]{5}\]="[^"]+"|^echo "PASS [a-z0-9]{5} [^"(]+' \
		"${here}"/*.bash "${here}"/utility/include/*.bash \
	| grep -v '^[^:]*steps-test\.bash:' \
	| sed -E 's/^([^:]+):(fTest( -q)?|fTest_Start|fTest_Skip) ([a-z0-9]{5}) "([^"]+)"$/\1\t\4\t\5/; s/^([^:]+):\[([a-z0-9]{5})\]="([^"]+)"$/\1\t\2\t\3/; s/^([^:]+):echo "PASS ([a-z0-9]{5}) (.*[^ ]) *$/\1\t\2\t\3/')
((${#idName[@]} >= 15)) || fFail "found ${#idName[@]} test IDs in cicd, want 15 or more"

if ((fails)); then echo "dedm7: ${fails} check(s) failed" >&2; exit 1; fi
echo "PASS dedm7 step and test lines"


##	History:
##		- 2026-10-25 JC: Created.
