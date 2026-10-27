#!/usr/bin/env bash

#  shellcheck disable=1091  ## 'Not following.' The include is found at run time.

##	Purpose: Test dedn4. The pre-push hook runs cicd's merge gate only for a
##		push to main that changes more than docs, per the Merge gate rule in
##		the shared CICD rules. It runs the real hook in a scratch repo, with a
##		stand-in cicd.bash that only says how it was called.
##	Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
##	SPDX-License-Identifier: MIT

set -Eeuo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${here}/utility/include/safe-rm.bash"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/test_shclw_hook_XXXXXX")"
touch "${scratch}/.shclw-hook-owned"
fCleanup(){
	if [[ -f "${scratch}/.shclw-hook-owned" ]]; then fSafeRm "${scratch}" >/dev/null; fi
	return 0
}
trap fCleanup EXIT
fails=0

repo="${scratch}/repo"
mkdir -p "${repo}/cicd/hooks" "${repo}/project" "${repo}/source"
cp "${here}/hooks/pre-push" "${repo}/cicd/hooks/pre-push"
printf '#!/usr/bin/env bash\necho "gate: $*"\n' > "${repo}/cicd/cicd.bash"
chmod +x "${repo}/cicd/cicd.bash" "${repo}/cicd/hooks/pre-push"
fGit(){ git -C "${repo}" -c user.name=t -c user.email=t@t -c commit.gpgsign=false "$@"; }
fGit init --quiet
echo a > "${repo}/README.md"; echo a > "${repo}/source/x.go"
fGit add -A; fGit commit --quiet -m base
base="$(fGit rev-parse HEAD)"
echo b > "${repo}/README.md"; echo b > "${repo}/project/backlog.md"; echo b > "${repo}/source/notes.md"
fGit add -A; fGit commit --quiet -m docs
docs="$(fGit rev-parse HEAD)"
echo c > "${repo}/source/x.go"
fGit add -A; fGit commit --quiet -m code
code="$(fGit rev-parse HEAD)"
zero=0000000000000000000000000000000000000000

## fExpect WANT LINE...: WANT is "gate" or "none"; each LINE is one ref update on the hook's stdin.
fExpect(){
	local want="$1" got out
	shift
	out="$(cd "${repo}" && printf '%s\n' "$@" | ./cicd/hooks/pre-push origin url 2>&1)" || { echo "dedn4: hook failed for '$*': ${out}" >&2; fails=$((fails + 1)); return 0; }
	case "${out}" in
		"gate: --merge-gate --quiet --no-remote") got=gate ;;
		"") got=none ;;
		*) got="odd output '${out}'" ;;
	esac
	if [[ "${got}" != "${want}" ]]; then echo "dedn4: '$*': got ${got}, want ${want}" >&2; fails=$((fails + 1)); fi
	return 0
}

fExpect gate "refs/heads/main ${code} refs/heads/main ${base}"
fExpect gate "refs/heads/main ${code} refs/heads/main ${docs}"
fExpect none "refs/heads/main ${docs} refs/heads/main ${base}"
fExpect none "refs/heads/dev ${code} refs/heads/dev ${base}"
fExpect none "refs/heads/x ${code} refs/heads/x ${zero}"
fExpect gate "refs/heads/main ${code} refs/heads/main ${zero}"
fExpect none "(delete) ${zero} refs/heads/main ${code}"
fExpect gate "refs/heads/main ${code} refs/heads/main 1111111111111111111111111111111111111111"
fExpect gate "refs/heads/dev ${docs} refs/heads/dev ${base}" "refs/heads/main ${code} refs/heads/main ${docs}"
fExpect none "refs/heads/dev ${code} refs/heads/dev ${base}" "refs/heads/main ${docs} refs/heads/main ${base}"
fExpect none

if ((fails)); then echo "dedn4: ${fails} case(s) wrong" >&2; exit 1; fi
echo "PASS dedn4 pre-push hook gates only a push to main that changes more than docs"
