#!/bin/bash
#  shellcheck disable=1091  ## 'Not following.' The includes are in the tree the box unpacked.
#  shellcheck disable=2154  ## 'referenced but not assigned.' cpuJobs and friends come from cpu-cap.bash.

##	- Purpose: Runs shclwrap's Go, Rust, Python and C tests and the shared cases
##	  on a Mac, from the tree remote-test.bash sends. One PASS, FAIL or SKIP
##	  line per test, by its ID. Exits 1 when any test failed.
##	- Plain bash 3.2, since that's the stock /bin/bash and the box may have no other.
##	- Syntax:
##	  remote-macos.bash --prep
##	    Fed on stdin before the tree is there. Makes the run's work folder and
##	    prints "WORK <folder>", or prints "CANT <reason>" and exits 3 when a
##	    tool the tests need isn't on the box.
##	  remote-macos.bash WORK SHCL3_REV SHCL2_REV PY3_REV [KEEP]
##	    Runs the tests from WORK/repo. KEEP=1 keeps WORK.

##	History: At bottom of script.

##	Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT

set -Eeuo pipefail

## A non-interactive ssh gets only the system PATH, so each tool is looked for
## where Homebrew, rustup and the Go installer put it. Sets goBin, cargoBin,
## pyBin and cantWhy.
fFindTools(){
	local p
	goBin=""; cargoBin=""; pyBin=""; cantWhy=""
	for p in /usr/local/bin/go /opt/homebrew/bin/go /usr/local/go/bin/go; do
		if [[ -x "${p}" ]]; then goBin="${p}"; break; fi
	done
	if [[ -x "${HOME}/.cargo/bin/cargo" ]]; then cargoBin="${HOME}/.cargo/bin/cargo"; fi
	## The pinned 3.14 first. The cases' runner needs 3.11, the README's floor.
	for p in /usr/local/bin/python3.14 /opt/homebrew/bin/python3.14 /usr/local/bin/python3 /opt/homebrew/bin/python3 /usr/bin/python3; do
		if [[ -x "${p}" ]] && "${p}" -c 'import sys; sys.exit(sys.version_info < (3, 11))' < /dev/null > /dev/null 2>&1; then pyBin="${p}"; break; fi
	done
	if   [[ -z "${goBin}" ]];    then cantWhy="no Go"
	elif [[ -z "${cargoBin}" ]]; then cantWhy="no cargo in ~/.cargo/bin"
	elif [[ -z "${pyBin}" ]];    then cantWhy="no Python 3.11 or newer"
	elif [[ ! -x /usr/bin/cc || ! -x /usr/bin/make ]]; then cantWhy="no Xcode command line tools"
	fi
	[[ -z "${cantWhy}" ]]
}

## The run's folder, ${HOST_TEMP}/shclwrap/<YYYYmmDD-HHMMSS>_<NNN>, as safe-rm.bash's fRunDir makes it.
fPrep(){
	local r s n p
	if ! fFindTools; then echo "CANT ${cantWhy}"; exit 3; fi
	r="${HOST_TEMP:-${TMPDIR:-/tmp}}"
	r="$(cd -P -- "${r}" && pwd -P)/shclwrap" || exit 1
	mkdir -p -- "${r}" || exit 1
	s="$(date +%Y%m%d-%H%M%S)"
	for ((n = 1; n < 1000; n++)); do
		printf -v p '%s/%s_%03d' "${r}" "${s}" "${n}"
		if mkdir -- "${p}" 2> /dev/null; then
			: > "${p}/.shclw-cicd-owned"
			echo "WORK ${p}"
			exit 0
		fi
	done
	exit 1
}

## Runs a command with its output to WORK/NAME.txt, for a converter to read,
## and shows the end of it when it fails. Its exit code is in stepRc.
fStep(){
	local name="$1"; shift
	stepRc=0
	"$@" > "${work}/${name}.txt" 2>&1 || stepRc=$?
	if ((stepRc)); then echo "--- ${name} exit ${stepRc}, last lines:"; tail -n 30 "${work}/${name}.txt"; fi
	return 0
}
## Feeds a file to a converter, which prints the test lines.
fLines(){
	local file="$1" script="$2"; shift 2
	"${pyBin}" -X utf8 "${repo}/cicd/${script}" "$@" < "${file}" || failed=$((failed + 1))
}
fCleanup(){
	cd / || true
	if [[ "${keep}" == 1 ]]; then echo "kept ${work}"; else fSafeRm "${work}" > /dev/null || true; fi
}

fRun(){
	work="$1"; local s3="$2" s2="$3" p3="$4"; keep="${5:-0}"
	repo="${work}/repo"
	local cases="${repo}/tests/cases" c="${repo}/source/c" o="${work}/cbuild"
	source "${repo}/cicd/utility/include/safe-rm.bash"
	source "${repo}/cicd/utility/include/cpu-cap.bash"
	[[ "${work}" == "${HOST_TEMP}/shclwrap/${work##*/}" && "${work##*/}" =~ ^[0-9]{8}-[0-9]{6}_[0-9]{3}$ && -f "${work}/.shclw-cicd-owned" ]] \
		|| { echo "FAIL dejgh not a run folder: ${work}"; exit 1; }
	echo "$$" > "${work}/run.pid"
	trap fCleanup EXIT
	trap 'exit 129' HUP INT TERM
	## Run folders a run cut off left behind, after 2 days.
	fRunDir_Sweep
	fFindTools || { echo "FAIL dejgh ${cantWhy}"; exit 1; }

	fLowPriority
	fCpuCap
	echo "Jobs: ${cpuJobs} of ${cpuCount} CPUs, load ${cpuLoad}$( ((cpuBusy)) && echo ' (busy)'), nice $(ps -o nice= -p $$ | tr -d ' ')"
	export PATH="${HOME}/.cargo/bin:${goBin%/*}:/usr/bin:/bin:/usr/sbin:/sbin"
	export TMPDIR="${work}/tmp" PYTHONPYCACHEPREFIX="${work}/pycache"
	mkdir -p "${TMPDIR}" "${o}"
	failed=0

	cd "${repo}/source/go"
	fStep go "${goBin}" test -count=1 -json -p "${cpuJobs}" ./...
	fLines "${work}/go.txt" go_test_lines.py --cases "${cases}"
	if ((stepRc)); then failed=$((failed + 1)); fi

	## Rust builds cold in the work folder. A target folder kept between runs
	## keeps test binaries whose CARGO_MANIFEST_DIR is the last run's folder.
	cd "${repo}/source/rust"
	fStep rust "${cargoBin}" test --quiet --locked -j "${cpuJobs}" --all-features --tests -- --format pretty
	fLines "${work}/rust.txt" rust_test_lines.py
	if ((stepRc)); then failed=$((failed + 1)); fi

	cd "${repo}/source/python"
	PYTHONPATH="${repo}/cicd/cache/shcl-py/${p3}" "${pyBin}" "${repo}/cicd/py_tests.py" tests || failed=$((failed + 1))
	PYTHONPATH="${repo}/cicd/cache/shcl-py/${p3}" "${pyBin}" "${repo}/cicd/c_cases.py" --lang Python --version 3 --cases "${cases}" \
		--drive "${pyBin}" "${repo}/source/python/shclw_drive.py" || failed=$((failed + 1))

	## Plain, with Apple's clang: macOS has no LeakSanitizer, and the C++ build
	## and the sanitized one are the Linux run's.
	cd "${repo}"
	fStep c-build make -s -C "${c}" -j "${cpuJobs}" CC=cc CXX=c++ OUT="${o}" \
		SHCL_DIR="${repo}/cicd/cache/shcl-c/${s3}" SHCL2_DIR="${repo}/cicd/cache/shcl-c/${s2}" \
		names "${o}/api-test-plain" "${o}/shclw-drive-plain"
	if ((stepRc)); then
		echo "FAIL dejgh C builds on macOS"; failed=$((failed + 1))
	else
		"${o}/api-test-plain" || failed=$((failed + 1))
		"${pyBin}" "${repo}/cicd/c_cases.py" --drive "${o}/shclw-drive-plain" --version 3 --cases "${cases}" || failed=$((failed + 1))
	fi

	((failed == 0))
}

if [[ "${1:-}" == --prep ]]; then fPrep; else fRun "$@"; fi


##	History:
##		- 2026-10-28 JC: Created. b26, or vmKt6m when b26 is away.
