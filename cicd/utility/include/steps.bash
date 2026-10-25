#!/usr/bin/env bash

##	Purpose: cicd's output helpers and step lines, per the CICD style guide in
##		the shared CICD rules (backlog 2026102416400040). Source this and call:
##			fEcho, fEcho_Clean    a "[ msg ]" line, or a plain one; blanks collapse
##			fSection TITLE        blank line, a 79-wide rule, then TITLE
##			fStep_Plan NAME...    every step, in run order
##			fStep_Off NAME WHY    turns a step off; WHY is the flag that did it
##			fStep_On NAME         true when the step runs
##			fStep_Summary         the steps that run, and why the rest are off
##			fStep_Start NAME      opens a step: "Step N of M; Starting NAME"
##			fStep_Pass            closes it with PASS
##			fStep_Fail            closes the open step, if any, with FAIL
##			fStep_AllPassed       fails unless every step that's on passed
##			fStep_Abort RC LINE CMD  the ERR trap's line, then FAIL for the open test and step
##			fTest [-q] ID NAME CMD...  runs CMD and prints its test line; -q drops CMD's stdout
##			fTest_Start ID NAME   opens a test whose checks run inline
##			fTest_Pass            "PASS ID NAME" for the open test
##			fTest_Fail            "FAIL ID NAME" for the open test, if any; for traps
##			fTest_Skip ID NAME WHY  "SKIP ID NAME, WHY"
##		Steps turned off aren't counted, so N and M come from the plan.
##		Test IDs follow the Testing rule in the shared CICD rules (backlog 2026102416400060).
##	Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT

declare -i _wasLastEchoBlank=0
fEcho_ResetBlankCounter(){ _wasLastEchoBlank=0; }
fEcho_Clean(){ if [[ -n "${1:-}" ]]; then echo -e "$*"; _wasLastEchoBlank=0; elif [[ "${_wasLastEchoBlank}" == 0 ]] && echo; then _wasLastEchoBlank=1; fi; }
fEcho(){       if [[ -n "$*"     ]]; then fEcho_Clean "[ $* ]"; else fEcho_Clean ""; fi; }
fEcho_Force(){ fEcho_ResetBlankCounter; fEcho "$*"; }
_letterbox="•••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••"
fSection(){ fEcho_Clean; fEcho_Clean "${_letterbox}"; fEcho "$*"; }

declare -a _stepNames=()
declare -A _stepOffBy=()
_stepNow=""
declare -i _stepsPassed=0 stepNum=0 stepTotal=0

fStep_Plan(){ _stepNames=("$@"); _stepOffBy=(); _stepsPassed=0; return 0; }
fStep_Off(){  _stepOffBy["$1"]="$2"; }
fStep_On(){   [[ -z "${_stepOffBy["$1"]+x}" ]]; }

## fStep_Num NAME: stepNum is NAME's place among the steps that are on, 0 if it
## isn't one, and stepTotal is how many are on.
fStep_Num(){
	local name
	stepNum=0; stepTotal=0
	for name in "${_stepNames[@]}"; do
		if fStep_On "${name}"; then
			stepTotal+=1
			if [[ "${name}" == "${1:-}" ]]; then stepNum=${stepTotal}; fi
		fi
	done
	return 0
}

## Local time to the hundredth of a second, with no fork where bash has EPOCHREALTIME.
fStep_Stamp(){
	local now="${EPOCHREALTIME:-}" frac
	if [[ -z "${now}" ]]; then _stepStamp="$(date +%Y%m%d-%H%M%S).00"; return 0; fi
	frac="${now#*[.,]}00"
	printf -v _stepStamp '%(%Y%m%d-%H%M%S)T.%s' "${now%[.,]*}" "${frac:0:2}"
}

fStep_Summary(){
	local name on="" label="Turned off ...: "
	fStep_Num ""
	for name in "${_stepNames[@]}"; do
		if fStep_On "${name}"; then on+="${on:+, }${name}"; fi
	done
	fEcho_Clean "Steps ........: ${stepTotal}: ${on:-none}"
	for name in "${_stepNames[@]}"; do
		if ! fStep_On "${name}"; then
			fEcho_Clean "${label}${name} (${_stepOffBy["${name}"]})"
			label="                "
		fi
	done
	if [[ "${label}" == "Turned off"* ]]; then fEcho_Clean "${label}none"; fi
	return 0
}

fStep_Start(){
	fStep_Num "$1"
	if ((! stepNum)); then echo "fStep_Start: '$1' isn't a step that's on" >&2; return 1; fi
	_stepNow="$1"
	fStep_Stamp
	fSection "${_stepStamp}: Step ${stepNum} of ${stepTotal}; Starting ${_stepNow}"
}

## fStep_End PASS|FAIL
fStep_End(){
	fStep_Num "${_stepNow}"
	fStep_Stamp
	fEcho "${_stepStamp}: Step ${stepNum} of ${stepTotal}; $1 ${_stepNow}"
	_stepNow=""
}
fStep_Pass(){
	if [[ -z "${_stepNow}" ]]; then echo "fStep_Pass: no step is open" >&2; return 1; fi
	fStep_End PASS; _stepsPassed+=1
}
## A subshell's ERR trap runs before its parent's, and only the parent closes the step.
fStep_Fail(){ if [[ -n "${_stepNow}" ]] && ((BASH_SUBSHELL == 0)); then fStep_End FAIL; fi; return 0; }

## trap 'fStep_Abort "$?" "${LINENO}" "${BASH_COMMAND}"' ERR
fStep_Abort(){
	{
		printf '\n[ CICD ABORTED (exit %s) at line %s: %s ]\n' "$1" "$2" "$3"
		if ((BASH_SUBSHELL == 0)); then fTest_Fail; fStep_Fail; echo; fi
	} >&2
	exit "$1"
}

_testId=""; _testName=""
fTest_Line(){  fEcho_Clean "$1 $2 $3"; }
fTest_Start(){ _testId="$1"; _testName="$2"; }
fTest_Pass(){
	if [[ -z "${_testId}" ]]; then echo "fTest_Pass: no test is open" >&2; return 1; fi
	fTest_Line PASS "${_testId}" "${_testName}"; _testId=""
}
fTest_Fail(){ if [[ -n "${_testId}" ]]; then fTest_Line FAIL "${_testId}" "${_testName}"; _testId=""; fi; return 0; }
fTest_Skip(){ fTest_Line SKIP "$1" "$2${3:+, $3}"; }

## The command runs on the right of ||, where set -e is off, so a bash function
## handed in has to return its own failure.
fTest(){
	local quiet=0 rc=0
	if [[ "${1:-}" == -q ]]; then quiet=1; shift; fi
	fTest_Start "$1" "$2"; shift 2
	if ((quiet)); then "$@" >/dev/null || rc=$?; else "$@" || rc=$?; fi
	if ((rc)); then fTest_Fail; return "${rc}"; fi
	fTest_Pass
}

fStep_AllPassed(){
	fStep_Num ""
	if ((_stepsPassed != stepTotal)); then echo "${_stepsPassed} of ${stepTotal} steps passed; a step that's on never ran" >&2; return 1; fi
}


##	History:
##		- 2026-10-25 JC: Created, from cicd.bash's output helpers. Step lines, a 79-wide rule, and the off list.
##		- 2026-10-25 JC: Test lines, PASS, FAIL or SKIP with the test's ID and name.
