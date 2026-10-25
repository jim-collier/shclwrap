#!/usr/bin/env bash

##	Purpose: Test decb9. A checkout with core.autocrlf=true, Git for Windows'
##		default, must write every tracked file byte for byte as committed.
##		The shared cases fail on CRLF, and case 027 needs its own CRLF kept.
##	Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
##	SPDX-License-Identifier: MIT

set -Eeuo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/test_shclw_eol_XXXXXX")"
touch "${scratch}/.shclw-eol-owned"
fCleanup(){
	if [[ -f "${scratch}/.shclw-eol-owned" ]]; then rm -rf -- "${scratch}"; fi
	return 0
}
trap fCleanup EXIT

git -C "${root}" -c core.autocrlf=true checkout-index --all --force --prefix="${scratch}/tree/"
git -C "${root}" ls-files --format='%(objectname) %(path)' > "${scratch}/want"
sed "s|^[^ ]* |${scratch}/tree/|" "${scratch}/want" | git -C "${root}" hash-object --no-filters --stdin-paths > "${scratch}/got"

bad="$(paste -d ' ' "${scratch}/got" "${scratch}/want" | awk '$1 != $2 {print $3}')"
if [[ -n "${bad}" ]]; then
	echo "decb9: $(wc -l <<< "${bad}") file(s) change line ends under core.autocrlf=true, such as:" >&2
	head -5 <<< "${bad}" >&2
	exit 1
fi
echo "PASS decb9 line ends under core.autocrlf=true ($(wc -l < "${scratch}/want") files)"
