#!/usr/bin/env bash
#  shellcheck disable=1091  ## 'source is valid here, but shellcheck doesn't know the path to it.'

##	Purpose: Move every 3.0 shcl pin to one commit, the newest on shcl's dev
##		by default. shclwrap tracks shcl's dev until 3.0.0-beta1 is tagged.
##		Rust, Go, the C and Python sha256s, and the version strings all move
##		together. The known findings list for the new commit still has to be
##		made by a leak run, since each line needs a reason.
##	Usage: bump-shcl.bash [REV]
##	Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
##	SPDX-License-Identifier: MIT

set -Eeuo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${here}/.." && pwd)"
url="https://github.com/yottacore/shcl"
raw="https://raw.githubusercontent.com/yottacore/shcl"
fDie(){ echo "bump-shcl: $*" >&2; exit 1; }

source "${here}/utility/include/safe-rm.bash"
source "${root}/source/c/shcl-pins.env"
old="${SHCL3_REV}"

rev="${1:-}"
if [[ -z "${rev}" ]]; then
	rev="$(git ls-remote "${url}" refs/heads/dev | cut -f1)"
	[[ -n "${rev}" ]] || fDie "couldn't read dev from ${url}"
fi
[[ "${rev}" =~ ^[0-9a-f]{40}$ ]] || fDie "need a full 40-character commit, got '${rev}'"
if [[ "${rev}" == "${old}" ]]; then echo "Already at ${rev}"; exit 0; fi

## A commit only the local repo has can't be fetched by cargo, go, or curl.
work="$(mktemp -d "${TMPDIR:-/tmp}/bump_shcl_XXXXXX")"
touch "${work}/.bump-shcl-owned"
fCleanup(){
	if [[ -f "${work}/.bump-shcl-owned" ]]; then fSafeRm "${work}" >/dev/null; fi
	return 0
}
trap fCleanup EXIT
curl -fsSL --retry 2 -o "${work}/shcl.h" "${raw}/${rev}/source/c/shcl.h" || fDie "couldn't fetch shcl.h at ${rev}; is it pushed?"
curl -fsSL --retry 2 -o "${work}/shcl.py" "${raw}/${rev}/source/python/shcl.py" || fDie "couldn't fetch shcl.py at ${rev}"
sumH="$(sha256sum "${work}/shcl.h" | cut -d' ' -f1)"
sumPy="$(sha256sum "${work}/shcl.py" | cut -d' ' -f1)"

fSub(){   ## fSub FILE FROM TO, failing when FROM isn't there
	local file="$1" from="$2" to="$3"
	grep -qF -- "${from}" "${file}" || fDie "${file}: no '${from}'"
	FROM="${from}" TO="${to}" perl -0pi -e 's/\Q$ENV{FROM}\E/$ENV{TO}/g' "${file}"
}

fSub "${root}/source/c/shcl-pins.env" "SHCL3_REV=\"${old}\"" "SHCL3_REV=\"${rev}\""
fSub "${root}/source/c/shcl-pins.env" "SHCL3_SHA256=\"${SHCL3_SHA256}\"" "SHCL3_SHA256=\"${sumH}\""
source "${root}/source/python/shcl-pins.env"
fSub "${root}/source/python/shcl-pins.env" "SHCL3_PY_REV=\"${SHCL3_PY_REV}\"" "SHCL3_PY_REV=\"${rev}\""
fSub "${root}/source/python/shcl-pins.env" "SHCL3_PY_SHA256=\"${SHCL3_PY_SHA256}\"" "SHCL3_PY_SHA256=\"${sumPy}\""

for file in source/c/shclw.h source/go/shclw3/shclw3.go source/rust/src/v3.rs; do
	fSub "${root}/${file}" "3.0.0-dev+${old:0:8}" "3.0.0-dev+${rev:0:8}"
done

fSub "${root}/source/rust/Cargo.toml" "rev = \"${old}\"" "rev = \"${rev}\""
## Resolves the changed rev into Cargo.lock and touches nothing else.
(cd "${root}/source/rust" && cargo metadata --format-version 1 > /dev/null)

(cd "${root}/source/go" && GOFLAGS=-mod=mod go get "github.com/yottacore/shcl/source/go/v2@${rev}" && go mod tidy)

echo "Moved the 3.0 pin from ${old:0:8} to ${rev:0:8}."
if [[ ! -f "${root}/source/c/shcl-known/${rev}.txt" ]]; then
	echo "source/c/shcl-known/${rev}.txt is missing. A leak run lists what it finds; give each line its reason, and drop ${old}.txt."
fi
