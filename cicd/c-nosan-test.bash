#!/usr/bin/env bash

#  shellcheck disable=1091  ## 'Not following.' The include is found at run time.

##	Purpose: Test deebf. On a host whose gcc-15 can't build the sanitizers,
##		such as a Mac, c-in-container.bash's test and fuzz modes build the C
##		plain and say so, rather than fail (2026102307090301). A host that can
##		still gets the sanitized build. Runs it in a stand-in tree with fake
##		gcc-15, g++-15 and make on PATH, and a c_cases.py and fuzz.py that only
##		say what they were given.
##	Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
##	SPDX-License-Identifier: MIT

set -Eeuo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${here}/.." && pwd)"
source "${here}/utility/include/safe-rm.bash"
source "${root}/source/c/shcl-pins.env"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/test_shclw_nosan_XXXXXX")"
touch "${scratch}/.shclw-nosan-owned"
fCleanup(){
	if [[ -f "${scratch}/.shclw-nosan-owned" ]]; then fSafeRm "${scratch}" >/dev/null; fi
	return 0
}
trap fCleanup EXIT
fFail(){ echo "deebf: $*" >&2; exit 1; }

tree="${scratch}/tree"
mkdir -p "${tree}/cicd/utility/include" "${tree}/source/c" "${tree}/source/python" "${scratch}/bin" "${scratch}/work"
cp "${here}/c-in-container.bash" "${here}/tool-pins.env" "${tree}/cicd/"
cp "${here}/utility/include/steps.bash" "${here}/utility/include/c-tests.bash" "${tree}/cicd/utility/include/"
cp "${root}/source/c/shcl-pins.env" "${tree}/source/c/"
cp "${root}/source/python/shcl-pins.env" "${tree}/source/python/"
mkdir -p "${tree}/cicd/cache/shcl-c/${SHCL2_REV}" "${tree}/cicd/cache/shcl-c/${SHCL3_REV}"
touch "${tree}/cicd/cache/shcl-c/${SHCL2_REV}/shcl.h" "${tree}/cicd/cache/shcl-c/${SHCL3_REV}/shcl.h"
printf 'import sys\nprint("cases", *sys.argv[1:])\n' > "${tree}/cicd/c_cases.py"
printf 'import sys\nprint("fuzz", *sys.argv[1:])\n' > "${tree}/cicd/fuzz.py"

## SAN_SHIM=no: the compiler refuses -fsanitize, as gcc does on Apple silicon.
cat > "${scratch}/bin/gcc-15" <<'EOF'
#!/usr/bin/env bash
out=""
while (($#)); do
	case "$1" in
		-dumpfullversion) echo 15.3.0; exit 0 ;;
		-fsanitize=*) [[ "${SAN_SHIM}" != no ]] || { echo "planted: no sanitizers here" >&2; exit 1; } ;;
		-o) out="$2"; shift ;;
	esac
	shift
done
if [[ -n "${out}" ]]; then printf '#!/bin/sh\nexit 0\n' > "${out}"; chmod +x "${out}"; fi
EOF
cp "${scratch}/bin/gcc-15" "${scratch}/bin/g++-15"
## Logs what it was asked to build, and makes each path target a program that says it ran.
cat > "${scratch}/bin/make" <<'EOF'
#!/usr/bin/env bash
echo "$*" >> "${MAKE_LOG}"
for a in "$@"; do
	[[ "${a}" == "${MAKE_OUT}"/* ]] || continue
	mkdir -p "${a%/*}"; printf '#!/bin/sh\necho "ran %s"\n' "${a##*/}" > "${a}"; chmod +x "${a}"
done
EOF
chmod +x "${scratch}/bin/gcc-15" "${scratch}/bin/g++-15" "${scratch}/bin/make"

fRun(){   ## fRun SAN MODE [ARGS...]
	local san="$1"; shift
	: > "${scratch}/make.log"
	env -u SHCLW_CICD_LOCK -u SHCLW_KEEP_TEMP PATH="${scratch}/bin:${PATH}" SAN_SHIM="${san}" MAKE_LOG="${scratch}/make.log" MAKE_OUT="${scratch}/work" \
		SHCLW_C_HOST=1 SHCLW_C_WORK="${scratch}/work" SHCLW_C_ARTIFACTS="${scratch}/artifacts" SHCLW_JOBS=1 \
		bash "${tree}/cicd/c-in-container.bash" "$@" > "${scratch}/out" 2>&1 \
		|| fFail "${san} $1: c-in-container.bash failed: $(cat "${scratch}/out")"
}
fSays(){ grep -q -- "$2" "${scratch}/out" || fFail "$1: no '$2' in: $(cat "${scratch}/out")"; }
fBuilt(){ grep -q -- "$2" "${scratch}/make.log" || fFail "$1: make wasn't asked for '$2': $(cat "${scratch}/make.log")"; }
fNotBuilt(){ if grep -q -- "$2" "${scratch}/make.log"; then fFail "$1: make was asked for '$2': $(cat "${scratch}/make.log")"; fi; }
w="${scratch}/work/c3"

fRun no test 3
fSays "plain test" "WARNING: gcc-15 here can't build or run the sanitized C, so C builds plain"
fSays "plain test" "^ran api-test-plain$"
fSays "plain test" "^ran api-test-cxx$"
fSays "plain test" "^cases --drive ${w}/shclw-drive-plain "
fBuilt "plain test" "${w}/api-test-plain ${w}/api-test-cxx drive-plain$"
fNotBuilt "plain test" " test drive$"

fRun no fuzz 3 shclw-drive --seconds 5
fSays "plain fuzz" "can't build or run the sanitized C"
fSays "plain fuzz" "^fuzz .* --c ${scratch}/work/c{v}/shclw-drive-plain "
fBuilt "plain fuzz" "${w}/shclw-drive-plain$"

fRun yes test 3
if grep -q "sanitized C" "${scratch}/out"; then fFail "sanitizers that work still warned: $(cat "${scratch}/out")"; fi
fBuilt "sanitized test" " test drive$"
fSays "sanitized test" "^cases --drive ${w}/shclw-drive "

fRun yes fuzz 3 shclw-drive --seconds 5
fSays "sanitized fuzz" "^fuzz .* --c ${scratch}/work/c{v}/shclw-drive "

echo "PASS deebf C builds plain on a host with no sanitizers"


##	History:
##		- 2026-10-25 JC: Created.
