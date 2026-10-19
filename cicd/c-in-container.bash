#!/usr/bin/env bash
# shellcheck disable=1091

set -Eeuo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${here}/.." && pwd)"
source "${here}/../source/c/shcl-pins.env"
source "${here}/tool-pins.env"

if (($# < 2)); then
	echo "usage: c-in-container.bash MODE VERSION [FUZZ ARGS...]" >&2
	exit 2
fi

mode="$1"
version="$2"
shift 2

[[ "$(gcc-15 -dumpfullversion)" == "${PIN_GCC15_CONTAINER}" ]] || { echo "gcc-15 is not ${PIN_GCC15_CONTAINER}" >&2; exit 1; }
[[ "$(clang-22 --version | sed -n 1p)" == *"${PIN_CLANG_TIDY}"* ]] || { echo "clang-22 is not ${PIN_CLANG_TIDY}" >&2; exit 1; }
[[ "$(clang-tidy-22 --version)" == *"${PIN_CLANG_TIDY}"* ]] || { echo "clang-tidy-22 is not ${PIN_CLANG_TIDY}" >&2; exit 1; }
[[ "$(clang-format-22 --version)" == *"${PIN_CLANG_TIDY}"* ]] || { echo "clang-format-22 is not ${PIN_CLANG_TIDY}" >&2; exit 1; }
[[ "$(heaptrack --version)" == *"${PIN_HEAPTRACK}"* ]] || { echo "heaptrack is not ${PIN_HEAPTRACK}" >&2; exit 1; }

shcl2Dir="${root}/cicd/cache/shcl-c/${SHCL2_REV}"
shcl3Dir="${root}/cicd/cache/shcl-c/${SHCL3_REV}"
[[ "${version}" == 3 ]] || { echo "version must be 3" >&2; exit 2; }
for header in "${shcl2Dir}/shcl.h" "${shcl3Dir}/shcl.h"; do
	[[ -f "${header}" ]] || { echo "missing pinned C header: ${header}" >&2; exit 1; }
done

## Half the cores, as in cicd.bash. Only builds get it; parallel lint output is a mess to read.
jobs=$(( $(nproc) / 2 )); if ((jobs < 1)); then jobs=1; fi

make_c(){
	make -s -C "${root}/source/c" \
		SHCL_DIR="${shcl3Dir}" SHCL2_DIR="${shcl2Dir}" OUT="/work/c${version}" \
		CC=gcc-15 CXX=g++-15 CC15=gcc-15 CLANG=clang-22 CLANG_TIDY=clang-tidy-22 CLANG_FORMAT=clang-format-22 "$@"
}

case "${mode}" in
	lint)
		make_c lint strict analyze format-check
		;;
	test)
		make_c -j "${jobs}" test drive
		LSAN_OPTIONS="suppressions=${root}/source/c/lsan.supp:print_suppressions=1" \
		ASAN_OPTIONS="detect_leaks=1:halt_on_error=1" \
		python3 "${here}/c_cases.py" --drive "/work/c${version}/shclw-drive" \
			--version "${version}" --cases "${root}/tests/cases"
		;;
	quick)
		## No sanitizers and no analyzers, which are most of the time.
		make_c -j "${jobs}" strict drive-plain "/work/c${version}/api-test-plain"
		"/work/c${version}/api-test-plain"
		python3 "${here}/c_cases.py" --drive "/work/c${version}/shclw-drive-plain" \
			--version "${version}" --cases "${root}/tests/cases"
		;;
	fuzz)
		## DRIVE is shclw-drive, or shclw-drive-plain under --quick. The test stage made them, so make only checks.
		drive="$1"; shift
		make_c -j "${jobs}" "/work/c${version}/${drive}"
		LSAN_OPTIONS="suppressions=${root}/source/c/lsan.supp" \
		ASAN_OPTIONS="detect_leaks=1:halt_on_error=1" \
		python3 "${here}/fuzz.py" --go /work/bin/go-drive --rust /work/bin/rust-drive --c "/work/c{v}/${drive}" \
			--cases "${root}/tests/cases" --work /work/fuzz --out /work/fuzz-fail "$@"
		;;
	leakcheck)
		;;
	leakcheck-resume)
		[[ -s "${root}/cicd/artifacts/leakcheck/c-leakcheck-v3/codeql.sarif" ]] || { echo "missing saved CodeQL report required for resume" >&2; exit 1; }
		;;
	esac

if [[ "${mode}" == leakcheck || "${mode}" == leakcheck-resume ]]; then
		artifact_dir="/artifacts/c-leakcheck-v${version}"
		mkdir -p "${artifact_dir}" /work/ctu
		if [[ "${mode}" == leakcheck ]]; then
		codeql_zip="/work/codeql-${PIN_CODEQL}.zip"
		curl -fsSL "https://github.com/github/codeql-cli-binaries/releases/download/v${PIN_CODEQL}/codeql-linux64.zip" -o "${codeql_zip}"
		echo "${PIN_CODEQL_SHA256}  ${codeql_zip}" | sha256sum -c -
		python3 - "${codeql_zip}" <<'PY'
import os, sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as archive:
    for item in archive.infolist():
        path = archive.extract(item, "/work")
        mode = (item.external_attr >> 16) & 0o777
        if mode:
            os.chmod(path, mode)
PY
		codeql="/work/codeql/codeql"
		[[ "$("${codeql}" version | sed -n '1p')" == *"${PIN_CODEQL}"* ]] || { echo "CodeQL CLI pin mismatch" >&2; exit 1; }
		git clone --quiet --filter=blob:none https://github.com/github/codeql.git /work/codeql-queries
		git -C /work/codeql-queries checkout --quiet "${PIN_CODEQL_QUERY_COMMIT}"
		queries="/work/codeql-queries/cpp/ql/src/codeql-suites/cpp-security-and-quality.qls"
		[[ -f "${queries}" ]] || { echo "pinned CodeQL C++ suite is missing" >&2; exit 1; }
		# api_test.c deliberately sends TMPDIR-based fixture paths through filesystem helpers.
		cat > /work/codeql-build.bash <<EOF
#!/usr/bin/env bash
set -Eeuo pipefail
case "\${1:-capture}" in
	validate)
		make -s -C ${root}/source/c SHCL_DIR=${shcl3Dir} SHCL2_DIR=${shcl2Dir} OUT=/work/codeql-build-v3 CC=gcc-15 CXX=g++-15 CC15=gcc-15 CLANG=clang-22 CLANG_TIDY=clang-tidy-22 drive-plain
		;;
	capture)
		make -s -C ${root}/source/c OUT=/work/codeql-build-v3 clean
		make -s -C ${root}/source/c SHCL_DIR=${shcl3Dir} SHCL2_DIR=${shcl2Dir} OUT=/work/codeql-build-v3 CC=gcc-15 CXX=g++-15 CC15=gcc-15 CLANG=clang-22 CLANG_TIDY=clang-tidy-22 drive-plain
		;;
	*) echo "unknown CodeQL build mode: \${1}" >&2; exit 2 ;;
esac
EOF
		chmod +x /work/codeql-build.bash
		bash -n /work/codeql-build.bash
		bash /work/codeql-build.bash validate
		"${codeql}" database create --language=cpp --source-root="${root}/source/c" \
			--command="bash /work/codeql-build.bash capture" \
			/work/codeql-db
		"${codeql}" database analyze /work/codeql-db "${queries}" \
			/work/codeql-queries/cpp/ql/src/AlertSuppression.ql \
			--format=sarif-latest --output="${artifact_dir}/codeql.sarif"
		fi
		jq -e '[.runs[].results[]? | select(([.suppressions[]?.kind] | index("inSource")) == null)] | length == 0' "${artifact_dir}/codeql.sarif" >/dev/null || { echo "CodeQL findings; see ${artifact_dir}/codeql.sarif" >&2; exit 1; }
		ctu_dir="/work/ctu/v3"
		mkdir -p "${ctu_dir}/ast"
		: > "${ctu_dir}/externalDefMap.txt"
		ctu_sources=(shclw.c info.c shclw3.c run.c shclw-drive.c shclw3_v2.c)
		for src in "${ctu_sources[@]}"; do
			inc="${shcl3Dir}"; [[ "${src}" == shclw3_v2.c ]] && inc="${shcl2Dir}"
			ast="${ctu_dir}/ast/${src//\//_}.ast"
			clang-22 -std=c11 -O1 -g -I"${root}/source/c" -I"${inc}" \
				-emit-ast "${root}/source/c/${src}" -o "${ast}"
			clang-extdef-mapping-22 "${ast}" >> "${ctu_dir}/externalDefMap.txt"
		done
		for src in "${ctu_sources[@]}"; do
			inc="${shcl3Dir}"; [[ "${src}" == shclw3_v2.c ]] && inc="${shcl2Dir}"
			clang-22 --analyze -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror \
				-I"${root}/source/c" -I"${inc}" \
				-Xclang -analyzer-config -Xclang experimental-enable-naive-ctu-analysis=true \
				-Xclang -analyzer-config -Xclang "ctu-dir=${ctu_dir}" -Xclang -analyzer-output=text \
				-o /dev/null "${root}/source/c/${src}" >"${artifact_dir}/ctu-v3-${src}.log" 2>&1
		done
		probe_dir="/work/c-leak-probes"
		mkdir -p "${probe_dir}"
		cat > "${probe_dir}/leak.c" <<'EOF'
#include <stdlib.h>
int main(void) { return malloc(64) == NULL; }
EOF
		gcc-15 -fsanitize=address,leak -g "${probe_dir}/leak.c" -o "${probe_dir}/leak"
		if "${probe_dir}/leak" >"${probe_dir}/leak.out" 2>&1; then echo "leak probe unexpectedly passed" >&2; exit 1; fi
		grep -q 'LeakSanitizer: detected memory leaks' "${probe_dir}/leak.out" || { cat "${probe_dir}/leak.out" >&2; exit 1; }
		cat > "${probe_dir}/null.c" <<'EOF'
int main(void) { volatile int *p = 0; return *p; }
EOF
		if gcc-15 -std=c17 -O1 -Wall -Wextra -Werror -fanalyzer -c -o /dev/null "${probe_dir}/null.c" >"${probe_dir}/null.out" 2>&1; then echo "null probe unexpectedly passed" >&2; exit 1; fi
		grep -q 'analyzer-null-dereference' "${probe_dir}/null.out" || { cat "${probe_dir}/null.out" >&2; exit 1; }
		cat > "${probe_dir}/growth.c" <<'EOF'
#include <stdlib.h>
void **volatile allocation_root;
int main(int argc, char **argv) { if (argc != 2) return 2; size_t n = (size_t)strtoul(argv[1], NULL, 10) * 65536u; allocation_root = malloc(n * sizeof *allocation_root); if (!allocation_root) return 2; for (size_t i = 0; i < n; ++i) { allocation_root[i] = malloc(1024); if (!allocation_root[i]) return 3; } return allocation_root[0] == NULL; }
EOF
		gcc-15 -O1 -g "${probe_dir}/growth.c" -o "${probe_dir}/growth"
		heaptrack -o "${probe_dir}/growth-n" "${probe_dir}/growth" 1 >/dev/null 2>&1
		heaptrack -o "${probe_dir}/growth-2n" "${probe_dir}/growth" 2 >/dev/null 2>&1
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
			OUT="/work/c3" CC=gcc-15 CXX=g++-15 CC15=gcc-15 CLANG=clang-22 \
			CLANG_TIDY=clang-tidy-22 drive-plain
		for rounds in 128 256; do
			out="${artifact_dir}/heaptrack-v3-${rounds}"
			SHCLW_SOAK_ROUNDS="${rounds}" heaptrack -o "${out}" "/work/c3/shclw-drive-plain" 3 \
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
