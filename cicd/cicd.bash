#!/usr/bin/env bash

#  shellcheck disable=1091  ## 'source is valid here, but shellcheck doesn't know the path to it.'
#  shellcheck disable=2016  ## 'Expressions don't expand in single quotes.' fDockerLabels greps for literal text.
#  shellcheck disable=2154  ## 'referenced but not assigned.' False hit on vars the sourced includes set, such as fRunLog's log fd.

##	- Purpose: Local CI/CD pipeline for the shcl wrappers.
##	- Steps, in this order (fail-fast, any error aborts before the next step).
##	  The output numbers only the ones that run, and the first section says
##	  which flag turned off each of the rest.
##	   - remote sync (fast-forward from upstream before anything is built)
##	   - format (in place; check-only under the 3 runs below that rewrite nothing)
##	   - lint (go vet, staticcheck, clippy, clang-tidy, gcc-15, shellcheck, ruff)
##	   - tests (shared cases in every language, Python's included)
##	   - leak checks (CodeQL, CTU analysis, and the heaptrack soak, for C)
##	   - fuzz (Rust, C, and Python against Go from generated cases, plus Go's own fuzzer)
##	- Syntax:
##	  cicd/cicd.bash [options]
##	  With no options it runs every stage, in about 10 minutes.
##	  Options:
##	   --quick         the fast checks only, in about a minute. No remote sync,
##	                   C analyzers, sanitizers, or leak checks, and a short fuzz.
##	                   Nothing that needs the network. C is skipped when a pinned
##	                   shcl.h isn't cached yet, and Python when shcl.py isn't.
##	   --long, --slow  everything. The same as no options here, since nothing takes hours.
##	   --leakcheck     add the leak checks to --quick. Needs the network.
##	   --no-leakcheck  skip the leak checks
##	  These 3 never sync, rewrite a tracked file or prompt:
##	   --backlog-run-quick [AREA...]  format check, lint, and the fast tests, in a few
##	                   minutes. AREA is go, rust, python, c or cicd, and picks what
##	                   the format, lint and test steps check. Default: all.
##	   --backlog-run-full  every check of the default run
##	   --merge-gate    every check of the default run; what the pre-push hook runs
##	                   on a push to main
##	   --gate          another name for --backlog-run-quick
##	   --ci            another name for --merge-gate
##	  Turning steps off, or picking where they run:
##	   --no-sync       skip the remote sync stage
##	   --no-format     skip the format stage
##	   --no-lint       skip the lint stage
##	   --no-tests      skip the tests stage
##	   --no-fuzz       skip the fuzz stage
##	   --no-container  run C, Go, Rust and Python on the host, skipping what it has
##	                   no tool for, and the leak checks. Each also runs on the host
##	                   when docker is missing or its image won't build, and says so.
##	   --no-remote     skip other hosts and VMs. There are none yet.
##	  Other:
##	   --keep-temp     keep the run's scratch folder, and print where it is
##	   --dry-run       show which stages would run, and stop
##	   --list          the same, plus the test IDs each stage would run, by the
##	                   flags given. A box missing a tool or a cached pin still
##	                   skips some of them at run time.
##	   -q, --quiet     only the step list up front, and no prompt
##	   -y, --yes       no prompt
##	   -h, --help      show this help
##	- Fuzz budgets, in seconds: SHCLW_FUZZ_SECONDS (default 60, 10 with --quick)
##	  for Rust and C against Go, and SHCLW_GOFUZZ_SECONDS (default 30, 0 with --quick)
##	  for Go's fuzzer. Failing fuzz cases go to cicd/artifacts/fuzz/, and the
##	  newest 10 sets are kept.
##	- Test scratch goes under $TMPDIR, in a folder the run makes and removes.
##	- Runs at nice 19 and idle IO. Parallel tools get half the CPUs, or a quarter
##	  when the 1-minute load is over half the CPU count, checked before each
##	  stage. The leak checks, sanitized C, and fuzz always get a quarter.
##	- One run per tree at a time. A second one, such as a push hook, waits for
##	  the first. --dry-run and --help don't.
##	- C builds against the shcl.h pinned in source/c/shcl-pins.env, fetched by
##	  commit into cicd/cache/ and checked by sha256. It takes 2.0's too, for
##	  the upgrade check. A cached header no pin names is dropped.
##	- Python runs against the shcl.py pinned in source/python/shcl-pins.env,
##	  fetched the same way. Its 2.0 copy, _shclw_shcl2.py, is checked against
##	  its pin. Nothing is installed; PYTHONPATH points at the cache.

##	History: At bottom of script.

##	Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT


set -Eeuo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${here}/.." && pwd)"
source "${here}/tool-pins.env"
source "${root}/source/c/shcl-pins.env"
source "${root}/source/python/shcl-pins.env"
source "${here}/utility/include/gfs-rotate.bash"
source "${here}/utility/include/run-lock.bash"
source "${here}/utility/include/cpu-cap.bash"
source "${here}/utility/include/steps.bash"
source "${here}/utility/include/c-tests.bash"
source "${here}/utility/include/docker-tidy.bash"
fLowPriority
stamp="$(date +%Y%m%d-%H%M%S)"

## rustup has to win over any system rust, or rust-toolchain.toml means nothing.
export PATH="${HOME}/.cargo/bin:${PATH}"

## A workstation is usually running something else.
fCpuCap

cmdLine="$*"
doQuick=0; doLeakcheck=""; doQuiet=0; assumeYes=0; dryRun=0; listTests=0; keepTemp=0
doSync=1; doFormat=1; doLint=1; doTests=1; doFuzz=1; doContainer=1
## checkOnly: format checks and nothing syncs or prompts. runName is the named run that set it.
checkOnly=0; runName=""; aliasNote=""
declare -a areas=()
## Which flag turned each step off, for the first section. The later flag wins.
syncOffBy=""; leakOffBy=""; fuzzOffBy=""
fNamedRun(){   ## fNamedRun FLAG: the 3 runs that rewrite nothing
	checkOnly=1; assumeYes=1; runName="$1"; doSync=0; syncOffBy="$1"
}
while (($#)); do case "$1" in
	--quick)    doQuick=1; doSync=0; syncOffBy="--quick"; shift ;;
	--long|--slow) shift ;;
	--leakcheck) doLeakcheck=1; shift ;;
	--no-leakcheck) doLeakcheck=0; leakOffBy="--no-leakcheck"; shift ;;
	--gate|--backlog-run-quick)
		if [[ "$1" == --gate ]]; then aliasNote="--gate is another name for --backlog-run-quick"; fi
		fNamedRun --backlog-run-quick; doQuick=1; doFuzz=0; fuzzOffBy="--backlog-run-quick"; shift
		while (($#)) && [[ "$1" != -* ]]; do
			case "$1" in go|rust|python|c|cicd) areas+=("$1") ;; *) echo "unknown area: $1 (go, rust, python, c or cicd)" >&2; exit 2 ;; esac
			shift
		done ;;
	--ci|--merge-gate)
		if [[ "$1" == --ci ]]; then aliasNote="--ci is another name for --merge-gate"; fi
		fNamedRun --merge-gate; shift ;;
	--backlog-run-full) fNamedRun "$1"; shift ;;
	-q|--quiet) doQuiet=1; assumeYes=1; shift ;;
	-y|--yes)   assumeYes=1; shift ;;
	--no-sync)  doSync=0; syncOffBy="$1"; shift ;;
	--no-format) doFormat=0; shift ;;
	--no-lint)  doLint=0; shift ;;
	--no-tests) doTests=0; shift ;;
	--no-fuzz)  doFuzz=0; fuzzOffBy="$1"; shift ;;
	--no-container) doContainer=0; shift ;;
	--no-remote) shift ;;
	--keep-temp) keepTemp=1; shift ;;
	--dry-run)  dryRun=1; shift ;;
	--list)     dryRun=1; listTests=1; shift ;;
	-h|--help)  sed -n '/^##	- Purpose:/,/^##	History:/p' "${BASH_SOURCE[0]}" | sed '$d; s/^##	\{0,1\}//'; exit 0 ;;
	*) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
esac; done
if [[ -z "${doLeakcheck}" ]]; then
	doLeakcheck=$( ((doQuick)) && echo 0 || echo 1)
	leakOffBy="--quick without --leakcheck"
	if [[ "${runName}" == --backlog-run-quick ]]; then leakOffBy="--backlog-run-quick without --leakcheck"; fi
fi
if ((doLeakcheck && ! doContainer)); then doLeakcheck=0; leakOffBy="--no-container"; fi
## c-checks.bash and lang-checks.bash run on the host with these.
if ((! doContainer)); then export SHCLW_C_HOST=1 SHCLW_LANG_HOST=1; fi
## c-checks.bash and lang-checks.bash leave off --rm with this.
if ((keepTemp)); then export SHCLW_KEEP_TEMP=1; fi
## Cargo.lock is tracked, so a run that rewrites nothing leaves it as it is.
export SHCLW_CARGO_LOCKED="${checkOnly}" SHCLW_AREAS="${areas[*]}"
## fArea NAME...: true when any NAME is one of the areas asked for, or none were.
fArea(){ local a n; ((${#areas[@]})) || return 0; for a in "${areas[@]}"; do for n in "$@"; do [[ "${a}" != "${n}" ]] || return 0; done; done; return 1; }

## Every step in run order. A flag that turns one off adds a line here.
fStep_Plan "Remote sync" "Format" "Lint" "Tests" "C leak checks" "Fuzz"
((doSync))      || fStep_Off "Remote sync"   "${syncOffBy}"
((doFormat))    || fStep_Off "Format"        "--no-format"
((doLint))      || fStep_Off "Lint"          "--no-lint"
((doTests))     || fStep_Off "Tests"         "--no-tests"
((doLeakcheck)) || fStep_Off "C leak checks" "${leakOffBy}"
((doFuzz))      || fStep_Off "Fuzz"          "${fuzzOffBy}"

fuzzSecs="${SHCLW_FUZZ_SECONDS:-$( ((doQuick)) && echo 10 || echo 60)}"
goFuzzSecs="${SHCLW_GOFUZZ_SECONDS:-$( ((doQuick)) && echo 0 || echo 30)}"
[[ "${fuzzSecs}" =~ ^[0-9]+$ ]]   || { echo "SHCLW_FUZZ_SECONDS is not a count" >&2; exit 2; }
[[ "${goFuzzSecs}" =~ ^[0-9]+$ ]] || { echo "SHCLW_GOFUZZ_SECONDS is not a count" >&2; exit 2; }

## Output helpers (fEcho, fEcho_Clean, fSection) and step lines are in utility/include/steps.bash.
fCpuNote(){ printf '%s of %s CPUs, load %s%s' "${cpuJobs}" "${cpuCount}" "${cpuLoad}" "$( ((cpuBusy)) && echo ' (busy)')"; }
## fStep NAME [hard]: opens the step, with the CPU cap checked again for it.
fStep(){ fStep_Start "$1"; fCpuCap "${2:-}"; fEcho_Clean "Jobs: $(fCpuNote)"; }
fDie(){ { fEcho_Force "FAILED: $*"; fTest_Fail; fStep_Fail; echo; } >&2; exit 1; }
trap 'fStep_Abort "$?" "${LINENO}" "${BASH_COMMAND}"' ERR

## Warns when a tool on this box isn't the pinned version.
fCheckPin(){   ## fCheckPin NAME PINNED ACTUAL
	if [[ "$3" != *"$2"* ]]; then fEcho "WARNING: $1 is '${3}', pinned ${2} in cicd/tool-pins.env"; fi
	return 0
}

## Fetches the pinned shcl.h into the gitignored cache and checks its sha256.
## Under --quick it only checks, and returns 1 when the header isn't cached.
## --leakcheck needs the network anyway, so it fetches.
fShclHeader(){   ## fShclHeader REV SHA256
	local rev="$1" sum="$2"
	local dir="${root}/cicd/cache/shcl-c/${rev}"
	local got=""
	if [[ -f "${dir}/shcl.h" ]]; then got="$(sha256sum "${dir}/shcl.h" | cut -d' ' -f1)"; fi
	if [[ "${got}" != "${sum}" ]]; then
		if ((doQuick && ! doLeakcheck)); then fEcho "WARNING: shcl.h at ${rev} isn't cached, and --quick doesn't fetch; skipping C"; return 1; fi
		mkdir -p "${dir}"
		curl -fsSL --retry 2 -o "${dir}/shcl.h.part" "https://raw.githubusercontent.com/yottacore/shcl/${rev}/source/c/shcl.h" \
			|| fDie "couldn't fetch shcl.h at ${rev}"
		got="$(sha256sum "${dir}/shcl.h.part" | cut -d' ' -f1)"
		[[ "${got}" == "${sum}" ]] || fDie "shcl.h at ${rev} has sha256 ${got}, and ${sum} is pinned"
		mv -f "${dir}/shcl.h.part" "${dir}/shcl.h"
	fi
	return 0
}

## Fetches the pinned shcl.py the way fShclHeader fetches shcl.h, and checks
## the 2.0 copy beside the wrapper. Returns 1 under --quick when it isn't cached.
fShclPy(){
	local dir="${root}/cicd/cache/shcl-py/${SHCL3_PY_REV}"
	local got=""
	got="$(sha256sum "${root}/source/python/_shclw_shcl2.py" | cut -d' ' -f1)"
	[[ "${got}" == "${SHCL2_PY_SHA256}" ]] || fDie "source/python/_shclw_shcl2.py has sha256 ${got}, and ${SHCL2_PY_SHA256} is pinned"
	got=""
	if [[ -f "${dir}/shcl.py" ]]; then got="$(sha256sum "${dir}/shcl.py" | cut -d' ' -f1)"; fi
	if [[ "${got}" != "${SHCL3_PY_SHA256}" ]]; then
		if ((doQuick && ! doLeakcheck)); then fEcho "WARNING: shcl.py at ${SHCL3_PY_REV} isn't cached, and --quick doesn't fetch; skipping Python"; return 1; fi
		mkdir -p "${dir}"
		curl -fsSL --retry 2 -o "${dir}/shcl.py.part" "https://raw.githubusercontent.com/yottacore/shcl/${SHCL3_PY_REV}/source/python/shcl.py" \
			|| fDie "couldn't fetch shcl.py at ${SHCL3_PY_REV}"
		got="$(sha256sum "${dir}/shcl.py.part" | cut -d' ' -f1)"
		[[ "${got}" == "${SHCL3_PY_SHA256}" ]] || fDie "shcl.py at ${SHCL3_PY_REV} has sha256 ${got}, and ${SHCL3_PY_SHA256} is pinned"
		mv -f "${dir}/shcl.py.part" "${dir}/shcl.py"
	fi
	return 0
}

## Drops cached headers no pin names any more. The folder goes too, unless it holds anything else.
fShclHeader_Prune(){
	local dir rev
	for dir in "${root}/cicd/cache/shcl-c"/*/; do
		rev="${dir%/}"; rev="${rev##*/}"
		[[ -d "${dir}" && "${rev}" =~ ^[0-9a-f]{40}$ && "${rev}" != "${SHCL2_REV}" && "${rev}" != "${SHCL3_REV}" ]] || continue
		rm -f -- "${dir}shcl.h" "${dir}shcl.h.part"
		if rmdir -- "${dir}" 2>/dev/null; then fEcho "Dropped the cached shcl.h at ${rev}, which no pin names"; fi
	done
	return 0
}

## Keeps the newest 10 sets of fuzz failures. Only folders a run marked as its own go.
fFuzzFail_Prune(){
	local dir i
	local -a owned=()
	for dir in "${root}/cicd/artifacts/fuzz"/*/; do
		if [[ -f "${dir}.shclw-cicd-owned" ]]; then owned+=("${dir%/}"); fi
	done
	for ((i = 0; i < ${#owned[@]} - 10; i++)); do
		if [[ -f "${owned[i]}/.shclw-cicd-owned" ]]; then rm -rf -- "${owned[i]}"; fi
	done
	return 0
}

## fuzz.py's SKIP lines, so its test names stay in one place.
fFuzzSkip(){ PYTHONPYCACHEPREFIX="${work}/pycache" python3 "${here}/fuzz.py" "--skip=$1"; }

## A .PHONY name with no rule makes make say "nothing to be done" and pass.
fPhonyRules(){
	local makeRules target
	local -a phonyTargets=()
	read -ra phonyTargets < <(sed -n 's/^\.PHONY:[[:space:]]*//p' "${root}/source/c/Makefile") || true
	((${#phonyTargets[@]})) || { echo "no .PHONY line in source/c/Makefile" >&2; return 1; }
	makeRules="$(grep -oE '^[A-Za-z0-9_-]+:' "${root}/source/c/Makefile")"
	for target in "${phonyTargets[@]}"; do
		grep -qxF -- "${target}:" <<<"${makeRules}" || { echo "source/c/Makefile: .PHONY names ${target}, which has no rule" >&2; return 1; }
	done
}
## The Lint and Tests steps' own checks. --list calls them too, under fTest_ListOnly.
fLint_Tests(){ if fArea c; then fTest debab "C Makefile .PHONY names have rules" fPhonyRules; fi; }
fCicdTests(){
	fTest -q de7yv "cicd flag mixes" bash "${here}/flags-test.bash"
	fTest -q deea7 "--list names every test a run would run, and runs nothing" bash "${here}/list-test.bash"
	fTest dece1 "fuzz corpus keeps its bytes" env PYTHONPYCACHEPREFIX="${work}/pycache" PYTHONWARNDEFAULTENCODING=1 PYTHONWARNINGS=error::EncodingWarning python3 "${here}/fuzz_test.py" -q LoadCorpus
	fTest dedph "fuzz judges the generator only from enough cases" env TMPDIR="${work}" PYTHONPYCACHEPREFIX="${work}/pycache" PYTHONWARNDEFAULTENCODING=1 PYTHONWARNINGS=error::EncodingWarning python3 "${here}/fuzz_test.py" -q Judge
	fTest dee8t "shared cases, language tests and fuzz print a test line each, by their own IDs" env TMPDIR="${work}" PYTHONPYCACHEPREFIX="${work}/pycache" PYTHONWARNDEFAULTENCODING=1 PYTHONWARNINGS=error::EncodingWarning python3 "${here}/test_lines_test.py" -q
	fTest -q decc9 "one cicd run per tree at a time" env TMPDIR="${work}" bash "${here}/lock-test.bash"
	fTest -q decdx "runs started in the same second get their own logs" env TMPDIR="${work}" bash "${here}/log-name-test.bash"
	fTest -q dee8b "lint-report doesn't read a path as a warning" env TMPDIR="${work}" bash "${here}/lint-report-test.bash"
	fTest -q decb9 "line ends under core.autocrlf=true" env TMPDIR="${work}" bash "${here}/eol-test.bash"
	fTest -q dedkv "CPU caps and low priority" bash "${here}/cpu-cap-test.bash"
	fTest -q dedm7 "step and test lines" bash "${here}/steps-test.bash"
	fTest -q dedn4 "pre-push hook gates only a push to main that changes more than docs" env TMPDIR="${work}" bash "${here}/hook-test.bash"
	fTest dedn5 "C containers carry the run's name and labels" fDockerLabels
	fTest -q dednv "C runs on the host without docker or its image" env TMPDIR="${work}" bash "${here}/c-host-test.bash"
	fTest -q dedpt "Go, Rust and Python run on the host without docker or their image" env TMPDIR="${work}" bash "${here}/lang-host-test.bash"
	fTest -q dedqv "a run's end removes its containers and stale images, and nothing else" env TMPDIR="${work}" bash "${here}/docker-tidy-test.bash"
	fTest -q dedta "stale headers and fuzz sets are pruned, and nothing else" env TMPDIR="${work}" bash "${here}/prune-test.bash"
}

## --list: the test IDs each step that's on would run, one line each, by the flags given.
fListStep(){
	local py=(env PYTHONDONTWRITEBYTECODE=1 python3) a
	local -a langs=()
	for a in go rust python c; do if fArea "${a}"; then langs+=("${a}"); fi; done
	case "$1" in
		Lint)
			fLint_Tests
			if fArea c && ((! doQuick)); then fCTest_List "${cTestsLint[@]}"; fi ;;
		Tests)
			if fArea go;   then "${py[@]}" "${here}/test_ids.py" tests go; fi
			if fArea rust; then "${py[@]}" "${here}/test_ids.py" tests rust; fi
			if fArea python; then
				fTest decd4 "Python tests and shared cases, encoding warnings as errors" :
				"${py[@]}" "${here}/test_ids.py" tests python
			fi
			if fArea cicd; then fCicdTests; fi
			if fArea c; then
				if ((doQuick)); then fCTest_List decbj; fi
				fCTest_List "${cTestsBuild[@]}"
				"${py[@]}" "${here}/test_ids.py" tests c
			fi
			if ((${#langs[@]})); then "${py[@]}" "${here}/test_ids.py" cases "${langs[@]}"; fi ;;
		"C leak checks") fCTest_List "${cTestsLeak[@]}" ;;
		Fuzz)
			if ((fuzzSecs > 0)); then "${py[@]}" "${here}/fuzz.py" --list; fi
			if ((goFuzzSecs > 0)); then fTest ddzw3 "Go's fuzzer finds no panic in Run" :; fi ;;
	esac
}
fListOne(){
	local ids
	fStep_On "$1" || return 0
	fStep_Num "$1"
	fSection "Step ${stepNum} of ${stepTotal}; $1, test IDs"
	ids="$(fListStep "$1")"
	printf '%s\n' "${ids:-(none)}"
}
fListTests(){ fTest_ListOnly 1; fStep_Each fListOne; fTest_ListOnly 0; }

## The first section. --quiet keeps only the step list, except under --dry-run.
fPreflight(){
	local mode ni
	fSection "shclwrap local CI/CD"
	if [[ -n "${aliasNote}" ]]; then fEcho_Clean "Note .........: ${aliasNote}"; fi
	if ((doQuiet && ! dryRun)); then fStep_Summary; return 0; fi
	fEcho_Clean "Repo root ....: ${root}"
	mode="$( ((doQuick)) && echo quick || echo full)"
	if [[ -n "${runName}" ]]; then mode="${runName#--}, ${mode}, format check only"; fi
	if ((${#areas[@]})); then mode+=", areas: ${areas[*]}"; fi
	fEcho_Clean "Mode .........: ${mode}"
	fEcho_Clean "Remote sync ..: $( ((doSync)) && echo 'fast-forward when behind; abort when diverged' || echo '(skipped)')"
	fEcho_Clean "Leak checks ..: $( ((doLeakcheck)) && echo 'CodeQL, CTU, heaptrack soak' || echo '(skipped)')"
	fEcho_Clean "Fuzz .........: $( ((doFuzz)) && echo "${fuzzSecs}s Rust, C, and Python against Go, ${goFuzzSecs}s Go fuzzer" || echo '(skipped)')"
	fEcho_Clean "CPU cap ......: $(fCpuNote)"
	## ps, since a BSD nice with no command only prints its usage.
	ni="$(ps -o ni= -p "$$" 2>/dev/null || true)"
	fEcho_Clean "Priority .....: nice ${ni// /}$(command -v ionice >/dev/null && echo ", IO $(ionice -p $$ 2>/dev/null)")"
	fEcho_Clean "Scratch ......: ${work}"
	fStep_Summary
}

## Each container is named and labeled by its run, so a run's containers can be found.
fDockerLabels(){
	local script runs named bad=0
	for script in "${here}"/*.bash; do
		[[ "${script##*/}" != cpu-cap-test.bash ]] || continue
		runs="$(grep -cE '^[^#]*docker[[:space:]]run ' "${script}" || true)"
		((runs)) || continue
		named="$(grep -cF -- '--name "${name}" --label dev.project=shclwrap --label "dev.run=${runId}"' "${script}" || true)"
		if [[ "${named}" != "${runs}" ]]; then echo "${script##*/}: ${runs} container run(s), ${named} named and labeled" >&2; bad=1; fi
	done
	return "${bad}"
}

## The run's scratch folder, with a marker so cleanup only ever removes its own.
fScratchName(){ work="${TMPDIR:-/tmp}/test_shclw_${stamp}$(printf '%02d' $((RANDOM % 100)))"; }
fScratchName
runPassed=0; consoleFd=""
## The run's end, on failure and Ctrl+C too: its containers, stale images, and its scratch folder,
## in one line. Ctrl+C has already taken tee down, so the line goes to the console instead.
fCleanup(){
	local line="" size=""
	trap '' PIPE INT
	if [[ ! -f "${work}/.shclw-cicd-owned" ]]; then return 0; fi
	fDocker_Tidy "${work}" "${runPassed}" "${keepTemp}" || true
	if ((keepTemp)); then
		line="kept the scratch folder ${work}"
	else
		size="$(du -sk "${work}" 2>/dev/null | cut -f1 || true)"
		rm -rf -- "${work}"
		line="removed the scratch folder${size:+ ($(fBytes $((size * 1000))))}"
	fi
	if [[ -n "${dockerNote}" ]]; then line+=", ${dockerNote}"; fi
	line="Cleanup: ${line}."
	## Plain echo, since fEcho_Clean returns 0 even when its write fails.
	if { echo "${line}"; } 2>/dev/null; then fEcho_ResetBlankCounter; fEcho_Clean 2>/dev/null || true
	elif [[ -n "${consoleFd}" ]]; then { echo "${line}" >&"${consoleFd}"; } 2>/dev/null || true
	fi
	return 0
}
trap fCleanup EXIT
## Ctrl+C takes tee down too, and a write to it would kill this shell before the cleanup runs.
trap 'trap "" PIPE; exit 130' INT

if ((dryRun)); then fPreflight; if ((listTests)); then fListTests; fi; fEcho_Clean; exit 0; fi

## Tee the whole run to a rotated log, for cicd/utility/lint-report.bash.
fRunLog "${root}/cicd/artifacts/lint" stamp logFd
fScratchName
exec {consoleFd}>&1
exec > >(tee -a "/dev/fd/${logFd}") 2>&1
exec {logFd}>&-

fPreflight
if ((! assumeYes)); then read -r -p "Proceed? (Ctrl+C aborts, Enter continues) " _; fi
fRunLock "${root}" "cicd.bash ${cmdLine}"
## Another tree's run can pick the same name under the same TMPDIR.
mkdir -p "${work%/*}"
for ((scratchTry = 0; ; scratchTry++)); do
	if mkdir -- "${work}" 2>/dev/null; then break; fi
	((scratchTry < 20)) || fDie "no free scratch folder name under ${TMPDIR:-/tmp}"
	fScratchName
done
touch "${work}/.shclw-cicd-owned"

#•••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••
if fStep_On "Remote sync"; then
	fStep "Remote sync"
	branch="$(git -C "${root}" symbolic-ref --quiet --short HEAD || true)"
	upstream="$(git -C "${root}" rev-parse --abbrev-ref --symbolic-full-name '@{upstream}' 2>/dev/null || true)"
	if [[ -z "${branch}" ]]; then
		fEcho "WARNING: detached HEAD; nothing to sync"
	elif [[ -z "${upstream}" ]]; then
		fEcho "WARNING: ${branch} tracks no upstream; nothing to sync"
	elif ! git -C "${root}" fetch --quiet 2>/dev/null; then
		fEcho "WARNING: fetch failed (offline?); continuing against the local tree"
	else
		read -r ahead behind < <(git -C "${root}" rev-list --left-right --count "HEAD...${upstream}")
		if   ((ahead && behind)); then fDie "${branch} and ${upstream} have diverged (${ahead} local, ${behind} remote); reconcile by hand"
		elif ((behind)); then
			## Stash-wrapped, since a fast-forward refuses to touch a modified file.
			dirty=0
			if ! git -C "${root}" diff --quiet || ! git -C "${root}" diff --cached --quiet; then dirty=1; fi
			if [[ -n "$(git -C "${root}" ls-files --others --exclude-standard || true)" ]]; then dirty=1; fi
			if ((dirty)); then git -C "${root}" stash push --quiet --include-untracked --message "cicd sync ${stamp}"; fi
			ff=0; if git -C "${root}" merge --ff-only --quiet "${upstream}"; then ff=1; fi
			if ((dirty)) && ! git -C "${root}" stash pop --quiet; then
				fDie "the local changes conflict with ${upstream}; they are safe in the stash - resolve, then 'git stash drop'"
			fi
			((ff)) || fDie "fast-forward from ${upstream} failed"
			fEcho "OK: fast-forwarded ${behind} commit(s) from ${upstream}"
		elif ((ahead)); then fEcho "OK: ${ahead} commit(s) ahead of ${upstream}, nothing to take"
		else                 fEcho "OK: level with ${upstream}"
		fi
	fi
	fStep_Pass
fi

## The pinned shcl.h and shcl.py, once, from whichever step needs them first.
## Sets doC, and doPy, which lang-checks.bash gets as SHCLW_PY.
shclFetched=0; doC=0; doPy=0
fShclFetch(){
	((! shclFetched)) || return 0
	shclFetched=1; doC=1; doPy=1
	fShclHeader "${SHCL2_REV}" "${SHCL2_SHA256}" || doC=0
	fShclHeader "${SHCL3_REV}" "${SHCL3_SHA256}" || doC=0
	fShclHeader_Prune
	fShclPy || doPy=0
	export SHCLW_PY="${doPy}"
	return 0
}

#•••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••
if fStep_On "Format"; then
	fStep "Format"
	if ((checkOnly)); then
		if fArea go rust; then bash "${here}/lang-checks.bash" format-check "${work}" || fDie "gofmt or cargo fmt would change files"; fi
		if fArea c; then make -s -C "${root}/source/c" format-check || fDie "clang-format would change files"; fi
		fEcho "OK: formatted"
	else
		bash "${here}/lang-checks.bash" format "${work}"
		make -s -C "${root}/source/c" format
		fEcho "OK: gofmt, cargo fmt, and clang-format applied"
	fi
	fStep_Pass
fi

#•••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••
if fStep_On "Lint"; then
	fStep "Lint"
	fCheckPin shellcheck   "${PIN_SHELLCHECK}"  "$(shellcheck --version 2>/dev/null | sed -n 2p || echo missing)"
	fCheckPin gcc          "${PIN_GCC}"         "$(gcc --version 2>/dev/null | sed -n 1p || echo missing)"
	fCheckPin clang-format "${PIN_CLANG_FORMAT}" "$(clang-format --version 2>/dev/null || echo missing)"
	## go vet, staticcheck, clippy, and ruff over cicd and source/python.
	if fArea go rust python cicd; then bash "${here}/lang-checks.bash" lint "${work}"; fi
	fShclFetch
	if ! fArea c; then
		:
	elif ((! doC)); then
		fCTest_Skip "no shcl.h" "${cTestsLint[@]}"
	elif ((doQuick)); then
		fCTest_Skip "--quick" "${cTestsAnalyze[@]}"
	else
		bash "${here}/c-checks.bash" lint 3 "${work}"
		fEcho "OK: clang-tidy, gcc-15 at C17, and static analyzers (C over shcl 3; shcl's own held to source/c/shcl-known/)"
	fi
	if fArea cicd; then
		## cache/ holds other projects' code, such as the module and crate checkouts.
		mapfile -t shellFiles < <(find "${root}/cicd" \( -path "${root}/cicd/cache" -o -path "${root}/cicd/artifacts" \) -prune \
			-o -type f \( -name '*.bash' -o -path '*/hooks/*' \) -print | sort)
		((${#shellFiles[@]})) || fDie "shellcheck found no files"
		shellcheck "${shellFiles[@]}"
		fEcho "OK: shellcheck (${#shellFiles[@]} files)"
	fi
	fLint_Tests
	fStep_Pass
fi

#•••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••
if fStep_On "Tests"; then
	fStep "Tests"
	fShclFetch
	## The Go, Rust and Python tests, and Python's shared cases (decd4).
	if fArea go rust python; then bash "${here}/lang-checks.bash" test "${work}"; fi
	if fArea cicd; then fCicdTests; fi
	if ! fArea c || ((! doC)); then
		:
	elif ((doQuick)); then
		bash "${here}/c-checks.bash" quick 3 "${work}"
		fEcho "OK: gcc-15 at C17, C API tests, and shared cases over shcl 3, plain build"
	else
		bash "${here}/c-checks.bash" test 3 "${work}"
		fEcho "OK: C API tests and shared cases over shcl 3, sanitized, plain, and as C++"
	fi
	fStep_Pass
fi

#•••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••
if fStep_On "C leak checks"; then
	fStep "C leak checks" hard
	fShclFetch
	bash "${here}/c-checks.bash" leakcheck 3 "${work}"
	fEcho "OK: CodeQL, clang CTU, and heaptrack soak"
	fStep_Pass
else
	fCTest_Skip "${leakOffBy}" "${cTestsLeak[@]}"
fi

#•••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••
if fStep_On "Fuzz"; then
	fStep "Fuzz" hard
	fShclFetch
	bash "${here}/lang-checks.bash" drivers "${work}"
	failDir="${root}/cicd/artifacts/fuzz/${stamp}"
	if ((fuzzSecs > 0 && doC)); then
		## Where the C drivers were built, in the container or on the host. Go's and Rust's run there too.
		cDrive=shclw-drive; if ((doQuick)); then cDrive=shclw-drive-plain; fi
		bash "${here}/c-checks.bash" fuzz 3 "${work}" "${cDrive}" --seconds "${fuzzSecs}" || {
			if [[ -d "${work}/fuzz-fail" ]]; then mkdir -p "${failDir%/*}"; cp -r "${work}/fuzz-fail" "${failDir}"; touch "${failDir}/.shclw-cicd-owned"; fFuzzFail_Prune; fi
			fDie "fuzz found failures; cases are in ${failDir}"
		}
	elif ((fuzzSecs > 0)); then
		bash "${here}/lang-checks.bash" fuzz "${work}" --seconds "${fuzzSecs}" || {
			if [[ -d "${work}/fuzz-fail" ]]; then mkdir -p "${failDir%/*}"; cp -r "${work}/fuzz-fail" "${failDir}"; touch "${failDir}/.shclw-cicd-owned"; fFuzzFail_Prune; fi
			fDie "fuzz found failures; cases are in ${failDir}"
		}
	else
		fFuzzSkip "SHCLW_FUZZ_SECONDS=0"
	fi
	if ((goFuzzSecs > 0)); then
		bash "${here}/lang-checks.bash" gofuzz "${work}" "${goFuzzSecs}"
	elif [[ -n "${SHCLW_GOFUZZ_SECONDS:-}" ]]; then
		fTest_Skip ddzw3 "Go's fuzzer finds no panic in Run" "SHCLW_GOFUZZ_SECONDS=0"
	else
		fTest_Skip ddzw3 "Go's fuzzer finds no panic in Run" "--quick"
	fi
	fEcho "OK: fuzz"
	fStep_Pass
else
	fFuzzSkip "${fuzzOffBy}"
	fTest_Skip ddzw3 "Go's fuzzer finds no panic in Run" "${fuzzOffBy}"
fi

fStep_AllPassed
runPassed=1
fSection "shclwrap CI/CD: done."
fEcho_Clean


##	History:
##		- 2026-10-15 JC: Created. Format, lint, shared cases, and Go-against-Rust fuzz for the Go and Rust wrappers.
##		- 2026-10-15 JC: C over shcl 3.0: clang-format, clang-tidy, gcc-15, sanitized API tests, a C++ build, and the shared cases.
##		- 2026-10-15 JC: The C 3.0 build fetches shcl 2.0's shcl.h too, for the upgrade check.
##		- 2026-10-15 JC: C over shcl 1.2 and 2.0 too, each linted, tested, and run on the shared cases, plus the SHCLW_SHCL pick check.
##		- 2026-10-19 JC: --gate is now --ci. --quick runs only the fast checks, and no options runs everything, leak checks included.
##		- 2026-10-19 JC: The fuzz runs C too, on all 3 shcl versions.
##		- 2026-10-19 JC: shcl 3.0 is the floor. C builds and tests over 3.0 only, and the SHCLW_SHCL pick check is gone.
##		- 2026-10-21 JC: --dry-run. Drops cached headers no pin names, and keeps the newest 10 fuzz failure sets. --quick --leakcheck fetches headers.
##		- 2026-10-21 JC: Python: lint, tests, shared cases, and fuzz.
##		- 2026-10-24 JC: A second run in the same tree waits for the first.
##		- 2026-10-24 JC: Two runs started in the same second get their own logs and scratch folders.
##		- 2026-10-25 JC: Step lines with Starting, PASS and FAIL. Steps turned off aren't counted, and the first section says why each is off.
##		- 2026-10-25 JC: nice 19 and idle IO. A quarter of the CPUs when busy, and for the hard stages. go vet, staticcheck, ruff, and Rust's test threads get the count too.
##		- 2026-10-25 JC: The standard flags: --backlog-run-quick with areas, --backlog-run-full and --merge-gate rewrite nothing, and --ci and --gate are other names. --no-format, --no-lint, --no-tests, --no-container, --no-remote, --keep-temp, --list, --long. The C container is named and labeled by run.
##		- 2026-10-25 JC: C runs on the host under --no-container, or when docker is missing or the C image won't build.
##		- 2026-10-25 JC: Go, Rust and Python run in their own pinned image through lang-checks.bash, with the same host fallback.
##		- 2026-10-25 JC: The run's end stops and removes its containers by label, drops stale images and week-old build cache, and says what went in one line.
##		- 2026-10-25 JC: Test lines for the shared cases, the fuzz and Go's fuzzer, and SKIP lines for them when the fuzz is off.
##		- 2026-10-25 JC: --list names the test IDs each step would run, and --dry-run stays the step list.
