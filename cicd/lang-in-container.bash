#!/usr/bin/env bash
# shellcheck disable=1091

##	Purpose: The Go, Rust and Python checks, run through lang-checks.bash, in
##		the language image or on the host. Modes:
##			format        gofmt -w and cargo fmt
##			format-check  the same, changing nothing
##			lint          go vet, staticcheck, clippy, ruff
##			test          the Go, Rust and Python tests, and Python's shared cases,
##			              a test line per test function and shared case in each
##			drivers       the Go and Rust fuzz drivers, into WORK/bin
##			gofuzz SECS   Go's own fuzzer
##			fuzz ARGS...  fuzz.py, Rust and Python against Go, with no C
##		SHCLW_AREAS picks go, rust, python and cicd; empty is all of them.
##		SHCLW_CARGO_LOCKED=1 adds --locked, and SHCLW_PY=1 runs Python.
##	Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
##	SPDX-License-Identifier: MIT

set -Eeuo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${here}/.." && pwd)"
source "${root}/source/python/shcl-pins.env"
source "${here}/tool-pins.env"
source "${here}/utility/include/steps.bash"
trap fTest_Fail EXIT

if (($# < 1)); then
	echo "usage: lang-in-container.bash MODE [ARGS...]" >&2
	exit 2
fi
mode="$1"
shift
jobs="${SHCLW_JOBS:-}"
[[ "${jobs}" =~ ^[1-9][0-9]*$ ]] || { echo "lang-in-container.bash: run it through lang-checks.bash, which sets SHCLW_JOBS" >&2; exit 2; }

host=0; if [[ "${SHCLW_LANG_HOST:-}" == 1 ]]; then host=1; fi
work=/work
if ((host)); then work="${SHCLW_LANG_WORK:?}"; fi
read -ra areas <<<"${SHCLW_AREAS:-}"
fArea(){ local a; ((${#areas[@]})) || return 0; for a in "${areas[@]}"; do [[ "${a}" != "$1" ]] || return 0; done; return 1; }
cargoLocked=(); if [[ "${SHCLW_CARGO_LOCKED:-}" == 1 ]]; then cargoLocked=(--locked); fi
pyPath="${root}/cicd/cache/shcl-py/${SHCL3_PY_REV}"
rustTarget="${CARGO_TARGET_DIR:-${root}/source/rust/target}"

## In the image a tool off its pin is fatal. On the host it's a warning, from lint only, as before.
fPin(){   ## fPin NAME PINNED ACTUAL
	if [[ "$3" == *"$2"* ]]; then return 0; fi
	if ((host)); then echo "WARNING: $1 is '${3%%$'\n'*}', pinned ${2} in cicd/tool-pins.env"; return 0; fi
	echo "$1 is '$3', not $2" >&2; exit 1
}
if ((! host)) || [[ "${mode}" == lint ]]; then
	fPin go          "${PIN_GO}"          "$(go version 2>/dev/null || echo missing)"
	fPin staticcheck "${PIN_STATICCHECK}" "$(staticcheck --version 2>/dev/null || echo missing)"
	fPin ruff        "${PIN_RUFF}"        "$(ruff --version 2>/dev/null || echo missing)"
	if ((! host)); then fPin python3 "${PIN_PYTHON_CONTAINER}" "$(python3 --version)"; fi
fi

case "${mode}" in
	format)
		if fArea go;   then (cd "${root}/source/go" && gofmt -w .); fi
		if fArea rust; then (cd "${root}/source/rust" && cargo fmt); fi
		;;
	format-check)
		if fArea go; then
			unformatted="$(cd "${root}/source/go" && gofmt -l .)"
			[[ -z "${unformatted}" ]] || { echo "gofmt would change: ${unformatted}" >&2; exit 1; }
		fi
		if fArea rust; then (cd "${root}/source/rust" && cargo fmt --check) || { echo "cargo fmt would change files" >&2; exit 1; }; fi
		;;
	lint)
		if fArea go; then
			(cd "${root}/source/go" && go vet -p "${jobs}" ./... && staticcheck ./...)
			fEcho "OK: go vet, staticcheck"
		fi
		if fArea rust; then
			(cd "${root}/source/rust" && cargo clippy --quiet "${cargoLocked[@]}" -j "${jobs}" --all-features --all-targets -- -D warnings -W clippy::pedantic)
			fEcho "OK: clippy"
		fi
		if fArea python || fArea cicd; then
			ruff check --quiet "${root}/cicd" "${root}/source/python"
			ruff format --check --quiet "${root}/cicd" "${root}/source/python"
			fEcho "OK: ruff"
		fi
		;;
	test)
		if fArea go; then
			## go test hides a passing test's output, so the test lines come from its JSON.
			(cd "${root}/source/go" && TMPDIR="${work}" go test -json -p "${jobs}" ./...) \
				| PYTHONPYCACHEPREFIX="${work}/pycache" python3 "${here}/go_test_lines.py" --cases "${root}/tests/cases"
			fEcho "OK: Go tests"
		fi
		if fArea rust; then
			## --tests leaves out the doc examples, which aren't tests. Pretty gives a line per test.
			(cd "${root}/source/rust" && TMPDIR="${work}" cargo test --quiet "${cargoLocked[@]}" -j "${jobs}" --all-features --tests -- --format pretty) \
				| PYTHONPYCACHEPREFIX="${work}/pycache" python3 "${here}/rust_test_lines.py"
			fEcho "OK: Rust tests"
		fi
		if ! fArea python; then
			:
		elif [[ "${SHCLW_PY:-}" == 1 ]]; then
			## A text read or write left to the locale's encoding fails here, not first on Windows.
			fTest_Start decd4 "Python tests and shared cases, encoding warnings as errors"
			export PYTHONWARNDEFAULTENCODING=1 PYTHONWARNINGS=error::EncodingWarning PYTHONPATH="${pyPath}" PYTHONPYCACHEPREFIX="${work}/pycache"
			(cd "${root}/source/python" && TMPDIR="${work}" python3 "${here}/py_tests.py" tests)
			python3 "${here}/c_cases.py" --lang Python --version 3 --cases "${root}/tests/cases" \
				--drive python3 "${root}/source/python/shclw_drive.py"
			unset PYTHONWARNDEFAULTENCODING PYTHONWARNINGS PYTHONPATH
			fTest_Pass
		else
			fTest_Skip decd4 "Python tests and shared cases, encoding warnings as errors" "no shcl.py"
		fi
		;;
	drivers)
		mkdir -p "${work}/bin"
		(cd "${root}/source/go" && go build -p "${jobs}" -o "${work}/bin/go-drive" ./cmd/shclw-drive)
		(cd "${root}/source/rust" && cargo build --quiet "${cargoLocked[@]}" -j "${jobs}" --release --all-features --bin shclw-drive)
		cp "${rustTarget}/release/shclw-drive" "${work}/bin/rust-drive"
		;;
	gofuzz)
		fTest_Start ddzw3 "Go's fuzzer finds no panic in Run"
		(cd "${root}/source/go" && TMPDIR="${work}" go test ./shclw/ -run '^$' -fuzz=FuzzRun -fuzztime="${1:?seconds}s" -p "${jobs}" -parallel "${jobs}")
		fTest_Pass
		;;
	fuzz)
		pyArgs=(); if [[ "${SHCLW_PY:-}" == 1 ]]; then pyArgs=(--python "${root}/source/python/shclw_drive.py" --python-path "${pyPath}"); fi
		PYTHONPYCACHEPREFIX="${work}/pycache" python3 "${here}/fuzz.py" --go "${work}/bin/go-drive" --rust "${work}/bin/rust-drive" "${pyArgs[@]}" \
			--cases "${root}/tests/cases" --work "${work}/fuzz" --out "${work}/fuzz-fail" "$@"
		;;
	*) echo "lang-in-container.bash: unknown mode ${mode}" >&2; exit 2 ;;
esac


##	History:
##		- 2026-10-25 JC: Created, from cicd.bash's Go, Rust and Python commands.
##		- 2026-10-25 JC: A test line per shared case in Go, and one for Go's fuzzer.
##		- 2026-10-25 JC: A test line per Go, Rust and Python test function.
