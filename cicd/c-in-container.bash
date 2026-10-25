#!/usr/bin/env bash
# shellcheck disable=1091
# shellcheck disable=2016  ## jq's $vars sit in single quotes.
# shellcheck disable=2154  ## cTests* come from c-tests.bash.

set -Eeuo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${here}/.." && pwd)"
source "${here}/../source/c/shcl-pins.env"
source "${here}/../source/python/shcl-pins.env"
source "${here}/tool-pins.env"
source "${here}/utility/include/steps.bash"
source "${here}/utility/include/c-tests.bash"
## A probe that exits 1 still gets its FAIL line.
trap fTest_Fail EXIT

if (($# < 2)); then
	echo "usage: c-in-container.bash MODE VERSION [FUZZ ARGS...]" >&2
	exit 2
fi

mode="$1"
version="$2"
shift 2

## In the image, a tool off its pin is fatal. On the host it's a warning, as
## cicd.bash's are, and a missing tool skips what needs it.
host=0; if [[ "${SHCLW_C_HOST:-}" == 1 ]]; then host=1; fi
work=/work; artifactsDir=/artifacts
if ((host)); then work="${SHCLW_C_WORK:?}"; artifactsDir="${SHCLW_C_ARTIFACTS:?}"; fi
## Each tool by the name the image has it under.
declare -A toolWant=([cc15]=gcc-15 [cxx]=g++-15 [clang]=clang-22 [clangTidy]=clang-tidy-22 [clangFormat]=clang-format-22
	[mingwCc]=x86_64-w64-mingw32-gcc-win32 [extdef]=clang-extdef-mapping-22 [heaptrack]=heaptrack)
## fTool VAR [HOST NAME]: the image's name for VAR, or on the host the other name, when on PATH.
fTool(){
	local n
	for n in "${toolWant[$1]}" "${2:-}"; do
		if [[ -n "${n}" ]] && command -v "${n}" >/dev/null; then echo "${n}"; return 0; fi
		((host)) || break
	done
	return 0
}
cc15="$(fTool cc15)";             cxx="$(fTool cxx g++)"
clang="$(fTool clang)";           clangTidy="$(fTool clangTidy clang-tidy)"
clangFormat="$(fTool clangFormat)"; mingwCc="$(fTool mingwCc)"
extdef="$(fTool extdef)";         heaptrack="$(fTool heaptrack)"
fPin(){   ## fPin TOOL WHAT ACTUAL
	if [[ -z "$1" || "$3" == *"$2"* ]]; then return 0; fi
	if ((host)); then echo "WARNING: $1 is '${3%%$'\n'*}', pinned ${2} in the C image"; return 0; fi
	echo "$1 is not $2" >&2; exit 1
}
fPin "${cc15}"        "${PIN_GCC15_CONTAINER}" "$("${cc15:-true}" -dumpfullversion)"
fPin "${cxx}"         "${PIN_GCC15_CONTAINER}" "$("${cxx:-true}" -dumpfullversion)"
fPin "${clang}"       "${PIN_CLANG_TIDY}"      "$("${clang:-true}" --version | sed -n 1p)"
fPin "${clangTidy}"   "${PIN_CLANG_TIDY}"      "$("${clangTidy:-true}" --version)"
fPin "${clangFormat}" "${PIN_CLANG_TIDY}"      "$("${clangFormat:-true}" --version)"
fPin "${heaptrack}"   "${PIN_HEAPTRACK}"       "$("${heaptrack:-true}" --version)"
fPin "${mingwCc}"     "${PIN_MINGW_CONTAINER}" "$("${mingwCc:-true}" -dumpfullversion)"
## fNeed WHAT TOOL...: true when every TOOL was found. Otherwise one line, and false.
fNeed(){
	local what="$1" t; local -a missing=(); shift
	for t in "$@"; do [[ -n "${!t}" ]] || missing+=("${toolWant[${t}]}"); done
	((${#missing[@]})) || return 0
	if ((! host)); then echo "the C image has no ${missing[*]}" >&2; exit 1; fi
	echo "WARNING: no ${missing[*]} on the host; skipping ${what}"; return 1
}

shcl2Dir="${root}/cicd/cache/shcl-c/${SHCL2_REV}"
shcl3Dir="${root}/cicd/cache/shcl-c/${SHCL3_REV}"
[[ "${version}" == 3 ]] || { echo "version must be 3" >&2; exit 2; }
for header in "${shcl2Dir}/shcl.h" "${shcl3Dir}/shcl.h"; do
	[[ -f "${header}" ]] || { echo "missing pinned C header: ${header}" >&2; exit 1; }
done

## From c-checks.bash, since nproc here sees every host CPU. Only builds get it; parallel lint output is a mess to read.
jobs="${SHCLW_JOBS:-}"
[[ "${jobs}" =~ ^[1-9][0-9]*$ ]] || { echo "SHCLW_JOBS is '${jobs}'; run this through c-checks.bash" >&2; exit 2; }

## fMingw [MAKE ARGS]: the Windows build, decbj.
fMingw(){
	if fNeed "the Windows build" mingwCc; then fCTest_Start decbj; make_c "$@" mingw; fTest_Pass; else fCTest_Skip "no mingw-w64 gcc on the host" decbj; fi
}
fLeakTools(){ fNeed "the C leak checks" cc15 cxx clang extdef heaptrack || { fCTest_Skip "no C leak tools on the host" "${cTestsLeak[@]}"; exit 0; }; }

make_c(){
	make -s -C "${root}/source/c" \
		SHCL_DIR="${shcl3Dir}" SHCL2_DIR="${shcl2Dir}" OUT="${work}/c${version}" \
		CC="${cc15}" CXX="${cxx}" CC15="${cc15}" CLANG="${clang}" CLANG_TIDY="${clangTidy}" CLANG_FORMAT="${clangFormat}" MINGW_CC="${mingwCc}" "$@"
}

case "${mode}" in
	lint)
		fNeed "C lint" cc15 || exit 0
		if fNeed "the analyzers and their probes" clang; then
			## The analyze-shcl rules must see a double free planted in a stand-in shcl.h.
			fCTest_Start de7zp
			probe_dir="${work}/shcl-analyze-probe"
			mkdir -p "${probe_dir}/inc"
			cat > "${probe_dir}/inc/shcl.h" <<'EOF'
#include <stdlib.h>
void shcl_probe_use(int *p);
#ifdef SHCL_IMPLEMENTATION
void shcl_probe(void) { int *p = malloc(sizeof *p); if (!p) return; shcl_probe_use(p); free(p); free(p); }
#endif
EOF
			make_c -j "${jobs}" analyze-shcl-report SHCL_DIR="${probe_dir}/inc" SHCL2_DIR="${probe_dir}/inc" SHCL_REPORT="${probe_dir}/out" >/dev/null
			## By check name at the planted line. clang 22 reworded clang 19's "Attempt to free released memory".
			for log in shcl3.gcc shcl3.clang shcl2.gcc shcl2.clang; do
				grep -qE '/shcl\.h:4:[0-9]+: warning: .*\[(-Wanalyzer-double-free|unix\.Malloc)\]' "${probe_dir}/out/${log}.txt" || { echo "analyze-shcl missed the planted double free (${log})" >&2; exit 1; }
			done
			fTest_Pass
			## The known list must fail on the planted finding, pass once it's listed, and fail once it's gone.
			fCTest_Start deb8c
			python3 "${here}/shcl_known.py" list gcc "${probe_dir}/out/shcl3.gcc.txt" > "${probe_dir}/known.txt"
			python3 "${here}/shcl_known.py" list clang "${probe_dir}/out/shcl3.clang.txt" >> "${probe_dir}/known.txt"
			make_c analyze-shcl SHCL_DIR="${probe_dir}/inc" SHCL2_DIR="${probe_dir}/inc" SHCL_REPORT="${probe_dir}/out" SHCL_KNOWN="${probe_dir}/known.txt" SHCL2_KNOWN="${probe_dir}/known.txt" >/dev/null
			: > "${probe_dir}/none.txt"
			if make_c analyze-shcl SHCL_DIR="${probe_dir}/inc" SHCL2_DIR="${probe_dir}/inc" SHCL_REPORT="${probe_dir}/out" SHCL_KNOWN="${probe_dir}/none.txt" SHCL2_KNOWN="${probe_dir}/known.txt" >/dev/null 2>&1; then echo "analyze-shcl passed a finding not in its list" >&2; exit 1; fi
			printf 'clang\tshcl.h:99:1\tcore.NullDereference\n' >> "${probe_dir}/known.txt"
			if make_c analyze-shcl SHCL_DIR="${probe_dir}/inc" SHCL2_DIR="${probe_dir}/inc" SHCL_REPORT="${probe_dir}/out" SHCL_KNOWN="${probe_dir}/known.txt" SHCL2_KNOWN="${probe_dir}/known.txt" >/dev/null 2>&1; then echo "analyze-shcl passed a listed finding that's gone" >&2; exit 1; fi
			fTest_Pass
			## make analyze's gcc pass must see a leak that -O1 optimizes away.
			fCTest_Start deba8
			cat > "${probe_dir}/leak.c" <<'EOF'
#include <stdlib.h>
#include <string.h>
int shclw_probe_leak(int flag);
int shclw_probe_leak(int flag) { char *p = malloc(64); if (!p) return -1; if (flag) return 1; memset(p, 0, 64); free(p); return 0; }
EOF
			if make_c analyze ANALYZE_C="${probe_dir}/leak.c" >"${probe_dir}/leak.out" 2>&1; then echo "make analyze passed a planted leak" >&2; exit 1; fi
			grep -q 'analyzer-malloc-leak' "${probe_dir}/leak.out" || { cat "${probe_dir}/leak.out" >&2; echo "gcc's analyzer missed the planted leak" >&2; exit 1; }
			fTest_Pass
			## A clang analyzer finding alone must fail make analyze. gcc is switched off.
			fCTest_Start decas
			if make_c analyze ANALYZE_C="${probe_dir}/leak.c" CC15=true >"${probe_dir}/leak-clang.out" 2>&1; then echo "make analyze passed a clang analyzer finding" >&2; exit 1; fi
			grep -q 'unix\.Malloc' "${probe_dir}/leak-clang.out" || { cat "${probe_dir}/leak-clang.out" >&2; echo "clang's analyzer missed the planted leak" >&2; exit 1; }
			fTest_Pass
			make_c -j "${jobs}" analyze-shcl SHCL_REPORT="${artifactsDir}/c-analyze-shcl"
			make_c analyze
		else
			fCTest_Skip "no clang-22 on the host" "${cTestsAnalyze[@]}"
		fi
		if fNeed "clang-tidy" clangTidy; then make_c lint; fi
		make_c strict
		## The Format step checks with the host's own clang-format.
		if fNeed "the clang-format-22 check" clangFormat; then make_c format-check; fi
		fMingw
		;;
	test)
		fNeed "the C tests" cc15 cxx || exit 0
		make_c -j "${jobs}" test drive
		LSAN_OPTIONS="suppressions=${root}/source/c/lsan.supp:print_suppressions=1" \
		ASAN_OPTIONS="detect_leaks=1:halt_on_error=1" \
		python3 "${here}/c_cases.py" --drive "${work}/c${version}/shclw-drive" \
			--version "${version}" --cases "${root}/tests/cases"
		;;
	quick)
		## No sanitizers and no analyzers, which are most of the time.
		fNeed "the C quick tests" cc15 || exit 0
		fMingw -j "${jobs}"
		make_c -j "${jobs}" strict drive-plain "${work}/c${version}/api-test-plain"
		"${work}/c${version}/api-test-plain"
		python3 "${here}/c_cases.py" --drive "${work}/c${version}/shclw-drive-plain" \
			--version "${version}" --cases "${root}/tests/cases"
		;;
	fuzz)
		## DRIVE is shclw-drive, or shclw-drive-plain under --quick. The test stage made them, so make only checks.
		drive="$1"; shift
		## A host with no gcc-15 still fuzzes the rest against Go.
		cArgs=()
		if fNeed "C in the fuzz" cc15; then make_c -j "${jobs}" "${work}/c${version}/${drive}"; cArgs=(--c "${work}/c{v}/${drive}"); fi
		## Python too, when cicd.bash had its pinned shcl.py.
		pyDir="${root}/cicd/cache/shcl-py/${SHCL3_PY_REV}"
		pyArgs=()
		if [[ -f "${pyDir}/shcl.py" && "$(sha256sum "${pyDir}/shcl.py" | cut -d' ' -f1)" == "${SHCL3_PY_SHA256}" ]]; then
			pyArgs=(--python "${root}/source/python/shclw_drive.py" --python-path "${pyDir}")
		fi
		LSAN_OPTIONS="suppressions=${root}/source/c/lsan.supp" \
		ASAN_OPTIONS="detect_leaks=1:halt_on_error=1" \
		PYTHONPYCACHEPREFIX="${work}/pycache" python3 "${here}/fuzz.py" --go "${work}/bin/go-drive" --rust "${work}/bin/rust-drive" "${cArgs[@]}" "${pyArgs[@]}" \
			--cases "${root}/tests/cases" --work "${work}/fuzz" --out "${work}/fuzz-fail" "$@"
		;;
	leakcheck)
		fLeakTools
		;;
	leakcheck-resume)
		fLeakTools
		[[ -s "${root}/cicd/artifacts/leakcheck/c-leakcheck-v3/codeql.sarif" ]] || { echo "missing saved CodeQL report required for resume" >&2; exit 1; }
		;;
	esac

if [[ "${mode}" == leakcheck || "${mode}" == leakcheck-resume ]]; then
		artifact_dir="${artifactsDir}/c-leakcheck-v${version}"
		mkdir -p "${artifact_dir}" "${work}/ctu"
		if [[ "${mode}" == leakcheck ]]; then
		# A fresh run's reports replace the last run's, including names it no longer writes.
		find "${artifact_dir}" -mindepth 1 -maxdepth 1 -type f -delete
		codeql_zip="${work}/codeql-${PIN_CODEQL}.zip"
		curl -fsSL "https://github.com/github/codeql-cli-binaries/releases/download/v${PIN_CODEQL}/codeql-linux64.zip" -o "${codeql_zip}"
		echo "${PIN_CODEQL_SHA256}  ${codeql_zip}" | sha256sum -c -
		python3 - "${codeql_zip}" "${work}" <<'PY'
import os, sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as archive:
    for item in archive.infolist():
        path = archive.extract(item, sys.argv[2])
        mode = (item.external_attr >> 16) & 0o777
        if mode:
            os.chmod(path, mode)
PY
		codeql="${work}/codeql/codeql"
		[[ "$("${codeql}" version | sed -n '1p')" == *"${PIN_CODEQL}"* ]] || { echo "CodeQL CLI pin mismatch" >&2; exit 1; }
		git clone --quiet --filter=blob:none https://github.com/github/codeql.git "${work}/codeql-queries"
		git -C "${work}/codeql-queries" checkout --quiet "${PIN_CODEQL_QUERY_COMMIT}"
		queries="${work}/codeql-queries/cpp/ql/src/codeql-suites/cpp-security-and-quality.qls"
		[[ -f "${queries}" ]] || { echo "pinned CodeQL C++ suite is missing" >&2; exit 1; }
		# api_test.c deliberately sends TMPDIR-based fixture paths through filesystem helpers.
		cat > "${work}/codeql-build.bash" <<EOF
#!/usr/bin/env bash
set -Eeuo pipefail
case "\${1:-capture}" in
	validate)
		make -s -C ${root}/source/c SHCL_DIR=${shcl3Dir} SHCL2_DIR=${shcl2Dir} OUT="${work}/codeql-build-v3" CC="${cc15}" CXX="${cxx}" CC15="${cc15}" CLANG="${clang}" CLANG_TIDY="${clangTidy}" drive-plain
		;;
	capture)
		make -s -C ${root}/source/c OUT="${work}/codeql-build-v3" clean
		make -s -C ${root}/source/c SHCL_DIR=${shcl3Dir} SHCL2_DIR=${shcl2Dir} OUT="${work}/codeql-build-v3" CC="${cc15}" CXX="${cxx}" CC15="${cc15}" CLANG="${clang}" CLANG_TIDY="${clangTidy}" drive-plain
		;;
	*) echo "unknown CodeQL build mode: \${1}" >&2; exit 2 ;;
esac
EOF
		chmod +x "${work}/codeql-build.bash"
		bash -n "${work}/codeql-build.bash"
		bash "${work}/codeql-build.bash" validate
		## The repo root, not source/c, so shcl.h under cicd/cache is inside it. CodeQL reports only on files there.
		"${codeql}" database create --threads="${jobs}" --language=cpp --source-root="${root}" \
			--command="bash ${work}/codeql-build.bash capture" \
			"${work}/codeql-db"
		"${codeql}" database analyze --threads="${jobs}" "${work}/codeql-db" "${queries}" \
			"${work}/codeql-queries/cpp/ql/src/AlertSuppression.ql" \
			--format=sarif-latest --output="${artifact_dir}/codeql.sarif"
		fi
		## Findings in shcl itself must match each pin's list in source/c/shcl-known/ (backlog 2026102321270148).
		openQuery='[.runs[].results[]? | select(([.suppressions[]?.kind] | index("inSource")) == null) | select((.locations[0].physicalLocation.artifactLocation.uri // "" | startswith("cicd/cache/")) == $shcl)] | length'
		python3 "${here}/shcl_known.py" codeql "${root}/source/c/shcl-known" "${artifact_dir}/codeql.sarif" "${SHCL3_REV}" "${SHCL2_REV}" || { echo "CodeQL findings in shcl differ from the known lists; see ${artifact_dir}/codeql.sarif" >&2; exit 1; }
		[[ "$(jq --argjson shcl false "${openQuery}" "${artifact_dir}/codeql.sarif")" == 0 ]] || { echo "CodeQL findings; see ${artifact_dir}/codeql.sarif" >&2; exit 1; }
		## A null dereference in a stand-in shcl.h, reached only through a call from
		## our side, must show in the CTU pass, and the check must hold it to the list.
		fCTest_Start deb9f
		probe_dir="${work}/ctu-probe"
		probe_inc="${probe_dir}/cicd/cache/shcl-c/probe"
		mkdir -p "${probe_inc}" "${probe_dir}/known" "${probe_dir}/ast"
		cat > "${probe_inc}/shcl.h" <<'EOF'
#ifndef SHCL_H
#define SHCL_H
int shcl_probe_get(const int *p);
#ifdef SHCL_IMPLEMENTATION
int shcl_probe_get(const int *p) { return *p; }
#endif
#endif
EOF
		printf '#include "shcl.h"\nint ours_probe(void) { return shcl_probe_get(0); }\n' > "${probe_dir}/ours.c"
		"${clang}" -std=c11 -I"${probe_inc}" -emit-ast "${probe_dir}/ours.c" -o "${probe_dir}/ast/ours.ast"
		"${clang}" -x c -std=c11 -DSHCL_IMPLEMENTATION -emit-ast "${probe_inc}/shcl.h" -o "${probe_dir}/ast/shcl.ast"
		{ "${extdef}" "${probe_dir}/ast/ours.ast"; "${extdef}" "${probe_dir}/ast/shcl.ast"; } > "${probe_dir}/ast/externalDefMap.txt"
		"${clang}" --analyze -std=c11 -I"${probe_inc}" \
			-Xclang -analyzer-config -Xclang experimental-enable-naive-ctu-analysis=true \
			-Xclang -analyzer-config -Xclang "ctu-dir=${probe_dir}/ast" -Xclang -analyzer-output=text \
			-o /dev/null "${probe_dir}/ours.c" >"${probe_dir}/ours.log" 2>&1
		grep -qE '/shcl\.h:5:[0-9]+: warning: .*\[core\.NullDereference\]' "${probe_dir}/ours.log" || { echo "CTU pass didn't follow the planted call into shcl.h" >&2; exit 1; }
		: > "${probe_dir}/known/probe.txt"
		if python3 "${here}/shcl_known.py" ctu "${probe_dir}/known" --rev probe "${probe_dir}/ours.log" >/dev/null 2>&1; then echo "CTU check passed a shcl finding not in its list" >&2; exit 1; fi
		python3 "${here}/shcl_known.py" list ctu "${probe_dir}/ours.log" --rev probe > "${probe_dir}/known/probe.txt"
		python3 "${here}/shcl_known.py" ctu "${probe_dir}/known" --rev probe "${probe_dir}/ours.log" >/dev/null
		printf '%s/ours.c:2:1: warning: planted [core.NullDereference]\n' "${probe_dir}" >> "${probe_dir}/ours.log"
		if python3 "${here}/shcl_known.py" ctu "${probe_dir}/known" --rev probe "${probe_dir}/ours.log" >/dev/null 2>&1; then echo "CTU check passed a finding of ours" >&2; exit 1; fi
		fTest_Pass
		ctu_dir="${work}/ctu/v3"
		mkdir -p "${ctu_dir}/ast"
		: > "${ctu_dir}/externalDefMap.txt"
		ctu_sources=(shclw.c info.c shclw3.c run.c shclw-drive.c shclw3_v2.c)
		for src in "${ctu_sources[@]}"; do
			inc="${shcl3Dir}"; [[ "${src}" == shclw3_v2.c ]] && inc="${shcl2Dir}"
			ast="${ctu_dir}/ast/${src//\//_}.ast"
			"${clang}" -std=c11 -O1 -g -I"${root}/source/c" -I"${inc}" \
				-emit-ast "${root}/source/c/${src}" -o "${ast}"
			"${extdef}" "${ast}" >> "${ctu_dir}/externalDefMap.txt"
		done
		## shcl too, so the pass follows our calls into it. The mapper takes only the main
		## file's definitions, so each shcl.h is the main file, built as its impl file builds it.
		"${clang}" -x c -std=c11 -O1 -g -DSHCL_IMPLEMENTATION \
			-emit-ast "${shcl3Dir}/shcl.h" -o "${ctu_dir}/ast/shcl3.ast"
		"${extdef}" "${ctu_dir}/ast/shcl3.ast" >> "${ctu_dir}/externalDefMap.txt"
		"${clang}" -x c -std=c11 -O1 -g -I"${root}/source/c" -include shclw3_v2_names.h -DSHCL_IMPLEMENTATION \
			-emit-ast "${shcl2Dir}/shcl.h" -o "${ctu_dir}/ast/shcl2.ast"
		"${extdef}" "${ctu_dir}/ast/shcl2.ast" >> "${ctu_dir}/externalDefMap.txt"
		for src in "${ctu_sources[@]}"; do
			inc="${shcl3Dir}"; [[ "${src}" == shclw3_v2.c ]] && inc="${shcl2Dir}"
			"${clang}" --analyze -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror \
				-I"${root}/source/c" -I"${inc}" \
				-Xclang -analyzer-config -Xclang experimental-enable-naive-ctu-analysis=true \
				-Xclang -analyzer-config -Xclang "ctu-dir=${ctu_dir}" -Xclang -analyzer-output=text \
				-o /dev/null "${root}/source/c/${src}" >"${artifact_dir}/ctu-v3-${src}.log" 2>&1
		done
		## Ours fail outright. Those inside shcl must match each pin's list, as CodeQL's do.
		ctu_logs=()
		for src in "${ctu_sources[@]}"; do ctu_logs+=("${artifact_dir}/ctu-v3-${src}.log"); done
		python3 "${here}/shcl_known.py" ctu "${root}/source/c/shcl-known" --rev "${SHCL3_REV}" --rev "${SHCL2_REV}" "${ctu_logs[@]}" \
			|| { echo "CTU findings; see ${artifact_dir}/ctu-v3-*.log" >&2; exit 1; }
		probe_dir="${work}/c-leak-probes"
		mkdir -p "${probe_dir}"
		cat > "${probe_dir}/leak.c" <<'EOF'
#include <stdlib.h>
int main(void) { return malloc(64) == NULL; }
EOF
		"${cc15}" -fsanitize=address,leak -g "${probe_dir}/leak.c" -o "${probe_dir}/leak"
		if "${probe_dir}/leak" >"${probe_dir}/leak.out" 2>&1; then echo "leak probe unexpectedly passed" >&2; exit 1; fi
		grep -q 'LeakSanitizer: detected memory leaks' "${probe_dir}/leak.out" || { cat "${probe_dir}/leak.out" >&2; exit 1; }
		cat > "${probe_dir}/null.c" <<'EOF'
int main(void) { volatile int *p = 0; return *p; }
EOF
		if "${cc15}" -std=c17 -O1 -Wall -Wextra -Werror -fanalyzer -c -o /dev/null "${probe_dir}/null.c" >"${probe_dir}/null.out" 2>&1; then echo "null probe unexpectedly passed" >&2; exit 1; fi
		grep -q 'analyzer-null-dereference' "${probe_dir}/null.out" || { cat "${probe_dir}/null.out" >&2; exit 1; }
		cat > "${probe_dir}/growth.c" <<'EOF'
#include <stdlib.h>
void **volatile allocation_root;
int main(int argc, char **argv) { if (argc != 2) return 2; size_t n = (size_t)strtoul(argv[1], NULL, 10) * 65536u; allocation_root = malloc(n * sizeof *allocation_root); if (!allocation_root) return 2; for (size_t i = 0; i < n; ++i) { allocation_root[i] = malloc(1024); if (!allocation_root[i]) return 3; } return allocation_root[0] == NULL; }
EOF
		"${cc15}" -O1 -g "${probe_dir}/growth.c" -o "${probe_dir}/growth"
		"${heaptrack}" -o "${probe_dir}/growth-n" "${probe_dir}/growth" 1 >/dev/null 2>&1
		"${heaptrack}" -o "${probe_dir}/growth-2n" "${probe_dir}/growth" 2 >/dev/null 2>&1
		heaptrack_print -f "${probe_dir}/growth-n.gz" --print-leaks >"${probe_dir}/growth-n.txt"
		heaptrack_print -f "${probe_dir}/growth-2n.gz" --print-leaks >"${probe_dir}/growth-2n.txt"
		python3 - "${probe_dir}/growth-n.txt" "${probe_dir}/growth-2n.txt" <<'PY'
import re, sys
def leaked(path):
    text = open(path, encoding="utf-8").read()
    match = re.search(r"total memory leaked:\s*([0-9.]+)([KMG]?)B?", text)
    if not match:
        raise SystemExit(f"no leak total in {path}")
    factor = {"": 1, "K": 1024, "M": 1024**2, "G": 1024**3}[match.group(2)]
    return float(match.group(1)) * factor
if leaked(sys.argv[2]) <= leaked(sys.argv[1]) + 65536:
    raise SystemExit("heap growth probe did not exceed the 64 KiB margin")
PY
		make -s -C "${root}/source/c" SHCL_DIR="${shcl3Dir}" SHCL2_DIR="${shcl2Dir}" \
			OUT="${work}/c3" CC="${cc15}" CXX="${cxx}" CC15="${cc15}" CLANG="${clang}" \
			CLANG_TIDY="${clangTidy}" drive-plain
		for rounds in 128 256; do
			out="${artifact_dir}/heaptrack-v3-${rounds}"
			SHCLW_SOAK_ROUNDS="${rounds}" "${heaptrack}" -o "${out}" "${work}/c3/shclw-drive-plain" 3 \
				"${root}/tests/cases/019-first-last/doc.shcl" <"${root}/tests/cases/019-first-last/ops.txt" \
				>"${probe_dir}/soak-v3-${rounds}.out"
			heaptrack_print -f "${out}.gz" --print-leaks >"${out}.txt"
		done
		python3 - "${artifact_dir}/heaptrack-v3-128.txt" "${artifact_dir}/heaptrack-v3-256.txt" <<'PY'
import re, sys
def leaked(path):
    text = open(path, encoding="utf-8").read()
    match = re.search(r"total memory leaked:\s*([0-9.]+)([KMG]?)B?", text)
    if not match:
        raise SystemExit(f"no leak total in {path}")
    factor = {"": 1, "K": 1024, "M": 1024**2, "G": 1024**3}[match.group(2)]
    return float(match.group(1)) * factor
if leaked(sys.argv[2]) > leaked(sys.argv[1]) + 65536:
    raise SystemExit("heap grew by more than 64 KiB from N to 2N")
PY
fi

if [[ "${mode}" != lint && "${mode}" != test && "${mode}" != quick && "${mode}" != fuzz && "${mode}" != leakcheck && "${mode}" != leakcheck-resume ]]; then
	echo "unknown mode: ${mode}" >&2
	exit 2
fi
