#!/usr/bin/env bash

#  shellcheck disable=1091  ## 'source is valid here, but shellcheck doesn't know the path to it.'
#  shellcheck disable=2154  ## 'referenced but not assigned.' False hit on trap strings that assign the var they use (rc=$?).

##	- Purpose: Local CI/CD pipeline for the shcl wrappers.
##	- Stages (fail-fast, any error aborts before the next stage):
##	   0. remote sync (fast-forward from upstream before anything is built)
##	   1. format (in place; check-only under --ci)
##	   2. lint (go vet, staticcheck, clippy, clang-tidy, gcc-15, shellcheck, ruff)
##	   3. tests (shared cases in every language, Python's included)
##	   4. leak checks (CodeQL, CTU analysis, and the heaptrack soak, for C)
##	   5. fuzz (Rust, C, and Python against Go from generated cases, plus Go's own fuzzer)
##	- Syntax:
##	  cicd/cicd.bash [options]
##	  With no options it runs every stage.
##	  Options:
##	   --ci            format check, lint, and tests only; what the pre-push hook runs
##	   --quick         the fast checks only, in about a minute. No remote sync,
##	                   C analyzers, sanitizers, or leak checks, and a short fuzz.
##	                   Nothing that needs the network. C is skipped when a pinned
##	                   shcl.h isn't cached yet, and Python when shcl.py isn't.
##	   --leakcheck     add the leak checks to --ci or --quick. Needs the network.
##	   --no-leakcheck  skip the leak checks
##	   -q, --quiet     no preflight block and no prompt
##	   -y, --yes       no prompt
##	   --no-sync       skip the remote sync stage
##	   --no-fuzz       skip the fuzz stage
##	   --dry-run       show which stages would run, and stop
##	   -h, --help      show this help
##	- Fuzz budgets, in seconds: SHCLW_FUZZ_SECONDS (default 60, 10 with --quick)
##	  for Rust and C against Go, and SHCLW_GOFUZZ_SECONDS (default 30, 0 with --quick)
##	  for Go's fuzzer. Failing fuzz cases go to cicd/artifacts/fuzz/, and the
##	  newest 10 sets are kept.
##	- Test scratch goes under $TMPDIR, in a folder the run makes and removes.
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
stamp="$(date +%Y%m%d-%H%M%S)"

## rustup has to win over any system rust, or rust-toolchain.toml means nothing.
export PATH="${HOME}/.cargo/bin:${PATH}"

## Half the cores, since a workstation is usually running something else.
cpuCap=$(( $(nproc 2>/dev/null || echo 2) / 2 )); if ((cpuCap < 1)); then cpuCap=1; fi

cmdLine="$*"
doCi=0; doQuick=0; doLeakcheck=""; doQuiet=0; assumeYes=0; doSync=1; doFuzz=1; dryRun=0
while (($#)); do case "$1" in
	--ci)       doCi=1; doFuzz=0; shift ;;
	--quick)    doQuick=1; doSync=0; shift ;;
	--leakcheck) doLeakcheck=1; shift ;;
	--no-leakcheck) doLeakcheck=0; shift ;;
	-q|--quiet) doQuiet=1; assumeYes=1; shift ;;
	-y|--yes)   assumeYes=1; shift ;;
	--no-sync)  doSync=0; shift ;;
	--no-fuzz)  doFuzz=0; shift ;;
	--dry-run)  dryRun=1; shift ;;
	-h|--help)  sed -n '/^##	- Purpose:/,/^##	History:/p' "${BASH_SOURCE[0]}" | sed '$d; s/^##	\{0,1\}//'; exit 0 ;;
	*) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
esac; done
if [[ -z "${doLeakcheck}" ]]; then doLeakcheck=$( ((doCi || doQuick)) && echo 0 || echo 1); fi

fuzzSecs="${SHCLW_FUZZ_SECONDS:-$( ((doQuick)) && echo 10 || echo 60)}"
goFuzzSecs="${SHCLW_GOFUZZ_SECONDS:-$( ((doQuick)) && echo 0 || echo 30)}"
[[ "${fuzzSecs}" =~ ^[0-9]+$ ]]   || { echo "SHCLW_FUZZ_SECONDS is not a count" >&2; exit 2; }
[[ "${goFuzzSecs}" =~ ^[0-9]+$ ]] || { echo "SHCLW_GOFUZZ_SECONDS is not a count" >&2; exit 2; }

## Output helpers: fEcho / fEcho_Clean, blank-collapsing.
declare -i _wasLastEchoBlank=0
fEcho_ResetBlankCounter(){ _wasLastEchoBlank=0; }
fEcho_Clean(){ if [[ -n "${1:-}" ]]; then echo -e "$*"; _wasLastEchoBlank=0; elif [[ "${_wasLastEchoBlank}" == 0 ]] && echo; then _wasLastEchoBlank=1; fi; }
fEcho(){       if [[ -n "$*"     ]]; then fEcho_Clean "[ $* ]"; else fEcho_Clean ""; fi; }
fEcho_Force(){ fEcho_ResetBlankCounter; fEcho "$*"; }
_letterbox="••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••"
fSection(){ fEcho_Clean; fEcho_Clean "${_letterbox}"; fEcho "$*"; }
fDie(){ { fEcho_Force "FAILED: $*"; echo; } >&2; exit 1; }
trap 'rc=$?; printf "\n[ CICD ABORTED (exit %s) at line %s: %s ]\n\n" "$rc" "$LINENO" "$BASH_COMMAND" >&2; exit $rc' ERR

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

fPreflight(){
	local mode
	fEcho_Clean
	fEcho_Clean "shclwrap local CI/CD"
	fEcho_Clean
	fEcho_Clean "Repo root ....: ${root}"
	if   ((doCi && doQuick)); then mode='ci, quick (format check, fast lint, fast tests)'
	elif ((doCi));            then mode='ci (format check, lint, tests)'
	elif ((doQuick));         then mode='quick (format, fast lint, fast tests, short fuzz)'
	else                           mode='full'
	fi
	fEcho_Clean "Mode .........: ${mode}"
	fEcho_Clean "Remote sync ..: $( ((doSync)) && echo 'fast-forward when behind; abort when diverged' || echo '(skipped)')"
	fEcho_Clean "Leak checks ..: $( ((doLeakcheck)) && echo 'CodeQL, CTU, heaptrack soak' || echo '(skipped)')"
	fEcho_Clean "Fuzz .........: $( ((doFuzz)) && echo "${fuzzSecs}s Rust, C, and Python against Go, ${goFuzzSecs}s Go fuzzer" || echo '(skipped)')"
	fEcho_Clean "CPU cap ......: ${cpuCap}"
	fEcho_Clean "Scratch ......: ${work}"
}

## The run's scratch folder, with a marker so cleanup only ever removes its own.
work="${TMPDIR:-/tmp}/test_shclw_${stamp}$(printf '%02d' $((RANDOM % 100)))"
fCleanup(){
	if [[ -f "${work}/.shclw-cicd-owned" ]]; then rm -rf -- "${work}"; fi
	return 0
}
trap fCleanup EXIT

if ((dryRun)); then fPreflight; fEcho_Clean; exit 0; fi

## Tee the whole run to a rotated log, for cicd/utility/lint-report.bash.
logDir="${root}/cicd/artifacts/lint"
mkdir -p "${logDir}"
exec > >(tee "${logDir}/run_${stamp}.log") 2>&1
gfs_rotate "${logDir}" run log >/dev/null 2>&1 || true

if ((! doQuiet)); then
	fPreflight
	if ((! assumeYes)); then read -r -p "Proceed? (Ctrl+C aborts, Enter continues) " _; fi
fi
fRunLock "${root}" "cicd.bash ${cmdLine}"
mkdir -p "${work}" && touch "${work}/.shclw-cicd-owned"

#•••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••
if ((doSync)); then
	fSection "0/5  Remote sync"
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
fi

#•••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••
fSection "1/5  Format"
if ((doCi)); then
	unformatted="$(cd "${root}/source/go" && gofmt -l .)"
	[[ -z "${unformatted}" ]] || fDie "gofmt would change: ${unformatted}"
	(cd "${root}/source/rust" && cargo fmt --check) || fDie "cargo fmt would change files"
	make -s -C "${root}/source/c" format-check || fDie "clang-format would change files"
	fEcho "OK: formatted"
else
	(cd "${root}/source/go" && gofmt -w .)
	(cd "${root}/source/rust" && cargo fmt)
	make -s -C "${root}/source/c" format
	fEcho "OK: gofmt, cargo fmt, and clang-format applied"
fi

#•••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••
fSection "2/5  Lint"
fCheckPin go           "${PIN_GO}"          "$(go version)"
fCheckPin staticcheck  "${PIN_STATICCHECK}" "$(staticcheck --version 2>/dev/null || echo missing)"
fCheckPin shellcheck   "${PIN_SHELLCHECK}"  "$(shellcheck --version 2>/dev/null | sed -n 2p || echo missing)"
fCheckPin ruff         "${PIN_RUFF}"        "$(ruff --version 2>/dev/null || echo missing)"
fCheckPin gcc          "${PIN_GCC}"         "$(gcc --version 2>/dev/null | sed -n 1p || echo missing)"
fCheckPin clang-format "${PIN_CLANG_FORMAT}" "$(clang-format --version 2>/dev/null || echo missing)"
(cd "${root}/source/go" && go vet ./... && staticcheck ./...)
fEcho "OK: go vet, staticcheck"
(cd "${root}/source/rust" && cargo clippy --quiet -j "${cpuCap}" --all-features --all-targets -- -D warnings -W clippy::pedantic)
fEcho "OK: clippy"
doC=1
fShclHeader "${SHCL2_REV}" "${SHCL2_SHA256}" || doC=0
fShclHeader "${SHCL3_REV}" "${SHCL3_SHA256}" || doC=0
fShclHeader_Prune
doPy=1
fShclPy || doPy=0
pyPath="${root}/cicd/cache/shcl-py/${SHCL3_PY_REV}"
if ((doC && ! doQuick)); then
	bash "${here}/c-checks.bash" lint 3 "${work}"
	fEcho "OK: clang-tidy, gcc-15 at C17, and static analyzers (C over shcl 3; shcl's own held to source/c/shcl-known/)"
fi
mapfile -t shellFiles < <(find "${root}/cicd" -type f \( -name '*.bash' -o -path '*/hooks/*' \) | sort)
((${#shellFiles[@]})) || fDie "shellcheck found no files"
shellcheck "${shellFiles[@]}"
fEcho "OK: shellcheck (${#shellFiles[@]} files)"
## Test vGBt9: a .PHONY name with no rule makes make say "nothing to be done" and pass.
read -ra phonyTargets < <(sed -n 's/^\.PHONY:[[:space:]]*//p' "${root}/source/c/Makefile") || true
((${#phonyTargets[@]})) || fDie "no .PHONY line in source/c/Makefile"
makeRules="$(grep -oE '^[A-Za-z0-9_-]+:' "${root}/source/c/Makefile")"
for target in "${phonyTargets[@]}"; do
	grep -qxF -- "${target}:" <<<"${makeRules}" || fDie "source/c/Makefile: .PHONY names ${target}, which has no rule"
done
fEcho "OK: C Makefile targets (${#phonyTargets[@]})"
ruff check --quiet "${root}/cicd" "${root}/source/python"
ruff format --check --quiet "${root}/cicd" "${root}/source/python"
fEcho "OK: ruff"

#•••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••
fSection "3/5  Tests"
(cd "${root}/source/go" && TMPDIR="${work}" go test -p "${cpuCap}" ./...)
fEcho "OK: Go tests"
(cd "${root}/source/rust" && TMPDIR="${work}" cargo test --quiet -j "${cpuCap}" --all-features)
fEcho "OK: Rust tests"
if ((doPy)); then
	## Test vGTH7: a text read or write left to the locale's encoding fails here, not first on Windows.
	export PYTHONWARNDEFAULTENCODING=1 PYTHONWARNINGS=error::EncodingWarning
	(cd "${root}/source/python" && TMPDIR="${work}" PYTHONPATH="${pyPath}" PYTHONPYCACHEPREFIX="${work}/pycache" python3 -m unittest discover -q -s tests)
	PYTHONPATH="${pyPath}" PYTHONPYCACHEPREFIX="${work}/pycache" python3 "${here}/c_cases.py" --lang Python --version 3 --cases "${root}/tests/cases" \
		--drive python3 "${root}/source/python/shclw_drive.py"
	unset PYTHONWARNDEFAULTENCODING PYTHONWARNINGS
	fEcho "OK: Python tests and shared cases"
fi
bash "${here}/flags-test.bash" >/dev/null
fEcho "OK: cicd flag mixes"
TMPDIR="${work}" bash "${here}/lock-test.bash" >/dev/null
fEcho "OK: one run per tree at a time"
TMPDIR="${work}" bash "${here}/eol-test.bash" >/dev/null
fEcho "OK: line ends under core.autocrlf=true"
if ((doC && doQuick)); then
	bash "${here}/c-checks.bash" quick 3 "${work}"
	fEcho "OK: gcc-15 at C17, C API tests, and shared cases over shcl 3, plain build"
elif ((doC)); then
	bash "${here}/c-checks.bash" test 3 "${work}"
	fEcho "OK: C API tests and shared cases over shcl 3, sanitized, plain, and as C++"
fi

#•••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••
if ((doLeakcheck)); then
	fSection "4/5  C leak checks"
	bash "${here}/c-checks.bash" leakcheck 3 "${work}"
	fEcho "OK: CodeQL, clang CTU, and heaptrack soak"
fi

#•••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••
if ((doFuzz)); then
	fSection "5/5  Fuzz"
	mkdir -p "${work}/bin"
	(cd "${root}/source/go" && go build -o "${work}/bin/go-drive" ./cmd/shclw-drive)
	(cd "${root}/source/rust" && cargo build --quiet -j "${cpuCap}" --release --all-features --bin shclw-drive)
	cp "${root}/source/rust/target/release/shclw-drive" "${work}/bin/rust-drive"
	failDir="${root}/cicd/artifacts/fuzz/${stamp}"
	if ((fuzzSecs > 0 && doC)); then
		## In the container, where the C drivers were built. Go's and Rust's run there too.
		cDrive=shclw-drive; if ((doQuick)); then cDrive=shclw-drive-plain; fi
		bash "${here}/c-checks.bash" fuzz 3 "${work}" "${cDrive}" --seconds "${fuzzSecs}" || {
			if [[ -d "${work}/fuzz-fail" ]]; then mkdir -p "${failDir%/*}"; cp -r "${work}/fuzz-fail" "${failDir}"; touch "${failDir}/.shclw-cicd-owned"; fFuzzFail_Prune; fi
			fDie "fuzz found failures; cases are in ${failDir}"
		}
	elif ((fuzzSecs > 0)); then
		pyArgs=(); if ((doPy)); then pyArgs=(--python "${root}/source/python/shclw_drive.py" --python-path "${pyPath}"); fi
		PYTHONPYCACHEPREFIX="${work}/pycache" python3 "${here}/fuzz.py" --go "${work}/bin/go-drive" --rust "${work}/bin/rust-drive" "${pyArgs[@]}" --seconds "${fuzzSecs}" \
			--cases "${root}/tests/cases" --work "${work}/fuzz" --out "${failDir}" || {
			if [[ -d "${failDir}" ]]; then touch "${failDir}/.shclw-cicd-owned"; fFuzzFail_Prune; fi
			fDie "fuzz found failures; cases are in ${failDir}"
		}
	fi
	if ((goFuzzSecs > 0)); then
		(cd "${root}/source/go" && TMPDIR="${work}" go test ./shclw/ -run '^$' -fuzz=FuzzRun -fuzztime="${goFuzzSecs}s" -parallel "${cpuCap}")
	fi
	fEcho "OK: fuzz"
fi

fEcho_Clean
fEcho_Clean "${_letterbox}"
fEcho "shclwrap CI/CD: done."
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
