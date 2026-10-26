#!/usr/bin/env bash

#  shellcheck disable=1091  ## 'Not following.' The include is found at run time.

##	Purpose: Test deeap. The profile step's two scripts (2026102416400080):
##		pprof2flame.py lays folded stacks out as an inferno-style SVG, and
##		flame-report.py reads the newest one back, by the time in its name.
##		Plain mode, which cicd runs, never touches the .flame-seen marker.
##	Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
##	SPDX-License-Identifier: MIT

set -Eeuo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${here}/utility/include/safe-rm.bash"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/test_shclw_profile_XXXXXX")"
touch "${scratch}/.shclw-profile-owned"
fCleanup(){
	if [[ -f "${scratch}/.shclw-profile-owned" ]]; then fSafeRm "${scratch}" >/dev/null; fi
	return 0
}
trap fCleanup EXIT
fFail(){ echo "deeap: $*" >&2; exit 1; }
export PYTHONPYCACHEPREFIX="${scratch}/pycache"

## fSvg NAME HOT COLD: main calls hot and cold, with that many samples each.
fSvg(){
	python3 - "${here}/utility/pprof2flame.py" "${scratch}/prof/$1" "$2" "$3" <<'PY'
import importlib.util, sys
spec = importlib.util.spec_from_file_location("pprof2flame", sys.argv[1])
p2f = importlib.util.module_from_spec(spec)
spec.loader.exec_module(p2f)
root = p2f.fTree({("main", "hot"): int(sys.argv[3]), ("main", "cold"): int(sys.argv[4])})
with open(sys.argv[2], "w", encoding="utf-8") as fh:
	fh.write(p2f.fEmit(p2f.fLayout(root), root[1], "test", 0))
PY
}
mkdir "${scratch}/prof"
fSvg flame_20261025-000002_frequent.svg 7 3
fSvg flame_20261024-000000_day.svg 1 9
fSvg flame_20261023-000000_zzzz.svg 1 1

report="$(python3 "${here}/utility/flame-report.py" --dir "${scratch}/prof")" || fFail "plain mode failed"
[[ "$(head -1 <<<"${report}")" == "FLAME flame_20261025-000002_frequent.svg  (20261025-000002, 10 samples)" ]] \
	|| fFail "didn't pick the newest by time: $(head -1 <<<"${report}")"
top="$(sed -n '/^top self-time/{n;p;}' <<<"${report}")"
[[ "${top}" =~ ^\ +70\.0%\ +7\ +hot$ ]] || fFail "top self-time is '${top}', not hot at 70%"
grep -qE '^ +30\.0% +3 +cold$' <<<"${report}" || fFail "no cold at 30%: ${report}"
[[ ! -e "${scratch}/prof/.flame-seen" ]] || fFail "plain mode wrote the marker"

[[ "$(python3 "${here}/utility/flame-report.py" --dir "${scratch}/prof" --check | head -1)" == "NEW flame_20261025-000002_frequent.svg"* ]] || fFail "--check didn't say NEW"
[[ "$(cat "${scratch}/prof/.flame-seen")" == 20261025-000002 ]] || fFail "--check didn't mark the newest"
[[ "$(python3 "${here}/utility/flame-report.py" --dir "${scratch}/prof" --check)" == "SEEN flame_20261025-000002_frequent.svg"* ]] || fFail "a second --check didn't say SEEN"

rc=0; python3 "${here}/utility/flame-report.py" --dir "${scratch}/none" 2>/dev/null || rc=$?
((rc == 2)) || fFail "a missing folder gave ${rc}, not 2"
echo "PASS deeap profile flamegraphs and their summary"


##	History:
##		- 2026-10-25 JC: Created.
