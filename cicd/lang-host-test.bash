#!/usr/bin/env bash

##	Purpose: Test dedpt. lang-checks.bash runs Go, Rust and Python on the host,
##		and says so in one line, when docker is missing or down, when the image
##		won't build, and under SHCLW_LANG_HOST=1 (cicd.bash --no-container),
##		which never calls docker. With a working docker it uses the image, with
##		the tree read-only except for the modes that write to it (2026102507474940).
##		Runs a copy of lang-checks.bash in a stand-in tree, with a fake docker on
##		PATH and a lang-in-container.bash that only says where it ran.
##	Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
##	SPDX-License-Identifier: MIT

set -Eeuo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/test_shclw_langhost_XXXXXX")"
touch "${scratch}/.shclw-langhost-owned"
fCleanup(){
	if [[ -f "${scratch}/.shclw-langhost-owned" ]]; then rm -rf -- "${scratch}"; fi
	return 0
}
trap fCleanup EXIT
fFail(){ echo "dedpt: $*" >&2; exit 1; }

tree="${scratch}/tree"
mkdir -p "${tree}/cicd/utility/include" "${tree}/source/rust" "${scratch}/bin" "${scratch}/work"
cp "${here}/lang-checks.bash" "${here}/tool-pins.env" "${tree}/cicd/"
cp "${here}/../source/rust/rust-toolchain.toml" "${tree}/source/rust/"
cp "${here}/utility/include/run-lock.bash" "${here}/utility/include/cpu-cap.bash" "${tree}/cicd/utility/include/"
cat > "${tree}/cicd/lang-in-container.bash" <<'EOF'
echo "ran host=${SHCLW_LANG_HOST:-0} work=${SHCLW_LANG_WORK:-} args=$*"
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

fRun(){   ## fRun SHIM MODE [VAR=VALUE...]: runs MODE with fuzz-like extra args.
	local shim="$1" mode="$2"; shift 2
	: > "${scratch}/docker.log"
	env -u SHCLW_CICD_LOCK -u SHCLW_LANG_HOST PATH="${scratch}/bin:${PATH}" DOCKER_SHIM="${shim}" DOCKER_LOG="${scratch}/docker.log" "$@" \
		bash "${tree}/cicd/lang-checks.bash" "${mode}" "${scratch}/work" --seconds 5 > "${scratch}/out" 2>&1 \
		|| fFail "${shim}: lang-checks.bash failed: $(cat "${scratch}/out")"
}
fHostOnce(){   ## fHostOnce CASE WHY: one host line with WHY, and the host run got the args.
	local lines
	lines="$(grep -c '^WARNING: Go, Rust and Python run on the host' "${scratch}/out" || true)"
	[[ "${lines}" == 1 ]] || fFail "$1: ${lines} host lines: $(cat "${scratch}/out")"
	grep -qxF "WARNING: Go, Rust and Python run on the host: $2" "${scratch}/out" || fFail "$1: wrong host line: $(cat "${scratch}/out")"
	grep -qxF "ran host=1 work=${scratch}/work args=fuzz --seconds 5" "${scratch}/out" || fFail "$1: didn't run on the host: $(cat "${scratch}/out")"
	if grep -q '^run ' "${scratch}/docker.log"; then fFail "$1: started a container too"; fi
}

fRun down fuzz
fHostOnce "no daemon" "no docker, or its daemon isn't running"

fRun nobuild fuzz
fHostOnce "failed build" "the Go, Rust and Python image didn't build; see cicd/artifacts/lang-image-build.log"
grep -q 'planted build failure' "${tree}/cicd/artifacts/lang-image-build.log" || fFail "the build's output isn't in its log"

fRun ok fuzz SHCLW_LANG_HOST=1
fHostOnce "--no-container" "--no-container"
[[ ! -s "${scratch}/docker.log" ]] || fFail "--no-container called docker: $(cat "${scratch}/docker.log")"

fRun ok fuzz SHCLW_AREAS="go rust"
if grep -q 'run on the host\|^ran host' "${scratch}/out"; then fFail "a working docker ran on the host: $(cat "${scratch}/out")"; fi
grep -qx 'ran in a container' "${scratch}/out" || fFail "a working docker didn't start the container: $(cat "${scratch}/out")"
runLine="$(grep '^run ' "${scratch}/docker.log")"
[[ "${runLine}" == *"dst=/src,readonly "* ]] || fFail "fuzz mounted the tree writable: ${runLine}"
[[ "${runLine}" == *"--env SHCLW_AREAS=go rust "* ]] || fFail "the areas didn't reach the container: ${runLine}"
[[ "${runLine}" == *"/cicd/lang-in-container.bash fuzz --seconds 5" ]] || fFail "the container didn't get the mode and args: ${runLine}"

fRun ok format
runLine="$(grep '^run ' "${scratch}/docker.log")"
[[ "${runLine}" == *"dst=/src "* ]] || fFail "format can't write the tree: ${runLine}"

echo "PASS dedpt Go, Rust and Python run on the host without docker or their image"


##	History:
##		- 2026-10-25 JC: Created.
