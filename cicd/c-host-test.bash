#!/usr/bin/env bash

##	Purpose: Test dednv. c-checks.bash runs C on the host, and says so in one
##		line, when docker is missing or down, when the C image won't build, and
##		under SHCLW_C_HOST=1 (cicd.bash --no-container), which never calls
##		docker. With a working docker it uses the container (2026102507474935).
##		Runs a copy of c-checks.bash in a stand-in tree, with a fake docker on
##		PATH and a c-in-container.bash that only says where it ran.
##	Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
##	SPDX-License-Identifier: MIT

set -Eeuo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/test_shclw_chost_XXXXXX")"
touch "${scratch}/.shclw-chost-owned"
fCleanup(){
	if [[ -f "${scratch}/.shclw-chost-owned" ]]; then rm -rf -- "${scratch}"; fi
	return 0
}
trap fCleanup EXIT
fFail(){ echo "dednv: $*" >&2; exit 1; }

tree="${scratch}/tree"
mkdir -p "${tree}/cicd/utility/include" "${scratch}/bin" "${scratch}/work"
cp "${here}/c-checks.bash" "${here}/tool-pins.env" "${tree}/cicd/"
cp "${here}/utility/include/run-lock.bash" "${here}/utility/include/cpu-cap.bash" "${tree}/cicd/utility/include/"
cat > "${tree}/cicd/c-in-container.bash" <<'EOF'
echo "ran host=${SHCLW_C_HOST:-0} work=${SHCLW_C_WORK:-} args=$*"
EOF
## DOCKER_SHIM: down (no daemon), nobuild (the build fails), or ok.
cat > "${scratch}/bin/docker" <<'EOF'
#!/usr/bin/env bash
echo "$*" >> "${DOCKER_LOG}"
[[ "${DOCKER_SHIM}" != down ]] || exit 1
case "$1" in
	build) [[ "${DOCKER_SHIM}" != nobuild ]] || { echo "planted build failure" >&2; exit 1; } ;;
	run) echo "ran in a container" ;;
	image) if [[ "$2" == inspect ]]; then echo sha256:0123456789abcdef0123; fi ;;
	container) exit 1 ;;
esac
exit 0
EOF
chmod +x "${scratch}/bin/docker"

fRun(){   ## fRun SHIM [VAR=VALUE...]: runs quick mode with fuzz-like extra args.
	local shim="$1"; shift
	: > "${scratch}/docker.log"
	env -u SHCLW_CICD_LOCK -u SHCLW_C_HOST PATH="${scratch}/bin:${PATH}" DOCKER_SHIM="${shim}" DOCKER_LOG="${scratch}/docker.log" "$@" \
		bash "${tree}/cicd/c-checks.bash" quick 3 "${scratch}/work" drv --seconds 5 > "${scratch}/out" 2>&1 \
		|| fFail "${shim}: c-checks.bash failed: $(cat "${scratch}/out")"
}
fHostOnce(){   ## fHostOnce CASE WHY: one host line with WHY, and the host run got the args.
	local lines
	lines="$(grep -c '^WARNING: C runs on the host' "${scratch}/out" || true)"
	[[ "${lines}" == 1 ]] || fFail "$1: ${lines} host lines: $(cat "${scratch}/out")"
	grep -qxF "WARNING: C runs on the host: $2" "${scratch}/out" || fFail "$1: wrong host line: $(cat "${scratch}/out")"
	grep -qxF "ran host=1 work=${scratch}/work args=quick 3 drv --seconds 5" "${scratch}/out" || fFail "$1: didn't run on the host: $(cat "${scratch}/out")"
	if grep -q '^run ' "${scratch}/docker.log"; then fFail "$1: started a container too"; fi
}

fRun down
fHostOnce "no daemon" "no docker, or its daemon isn't running"

fRun nobuild
fHostOnce "failed build" "the C image didn't build; see cicd/artifacts/c-image-build.log"
grep -q 'planted build failure' "${tree}/cicd/artifacts/c-image-build.log" || fFail "the build's output isn't in its log"

fRun ok SHCLW_C_HOST=1
fHostOnce "--no-container" "--no-container"
[[ ! -s "${scratch}/docker.log" ]] || fFail "--no-container called docker: $(cat "${scratch}/docker.log")"

fRun ok
if grep -q 'C runs on the host\|^ran host' "${scratch}/out"; then fFail "a working docker ran on the host: $(cat "${scratch}/out")"; fi
grep -qx 'ran in a container' "${scratch}/out" || fFail "a working docker didn't start the container: $(cat "${scratch}/out")"

echo "PASS dednv C runs on the host without docker or its image"


##	History:
##		- 2026-10-25 JC: Created.
