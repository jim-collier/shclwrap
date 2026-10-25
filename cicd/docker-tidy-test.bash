#!/usr/bin/env bash
#  shellcheck disable=1091  ## 'Not following.' The include is found at run time.
#  shellcheck disable=2016  ## 'Expressions don't expand in single quotes.' The greps look for literal text.

##	Purpose: Test dedqv. The end of a run stops and removes only that run's
##		containers, keeps them stopped under --keep-temp, removes the project's
##		dangling images and, after a pass, tags older than the one before the
##		current, skips an image a container uses, prunes build cache only
##		after a rebuild and only past a week, and says what went
##		(2026102416400020). Kept cache folders are emptied whole when over the
##		cap or filled by another toolchain. Runs utility/include/docker-tidy.bash
##		against a fake docker, and checks cicd's own docker calls by text.
##	Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
##	SPDX-License-Identifier: MIT

set -Eeuo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/test_shclw_tidy_XXXXXX")"
touch "${scratch}/.shclw-tidy-owned"
fCleanup(){
	if [[ -f "${scratch}/.shclw-tidy-owned" ]]; then rm -rf -- "${scratch}"; fi
	return 0
}
trap fCleanup EXIT
fFail(){ echo "dedqv: $*" >&2; exit 1; }

source "${here}/utility/include/docker-tidy.bash"
mkdir -p "${scratch}/bin" "${scratch}/work"
## Containers c1 and c2 are the run's. Images: NEW is the current tag, P1 the one
## before it, O1 and O2 older, D1 dangling, OLD what this run's build replaced.
cat > "${scratch}/bin/docker" <<'SHIM'
#!/usr/bin/env bash
echo "$*" >> "${DOCKER_LOG}"
case "$1 $2" in
	"version "*) exit 0 ;;
	"ps --all")
		if [[ "$*" == *"label=dev.run=work"* && -n "${FAKE_CTRS}" ]]; then printf '%s\n' ${FAKE_CTRS}; fi
		if [[ -n "${FAKE_INUSE}" && "$*" == *"ancestor=${FAKE_INUSE}"* ]]; then echo c9; fi ;;
	"image ls")
		if [[ "$*" == *"dangling=true"* ]]; then if [[ -n "${FAKE_DANGLING}" ]]; then printf '%s\n' ${FAKE_DANGLING}; fi
		else printf '%s\n' "cur NEW" "prev P1" "older O1" "oldest O2"; fi ;;
	"image inspect")
		if [[ "$*" == *"RootFS"*"repo:cur" ]]; then echo '["L1"]'
		elif [[ "$*" == *"repo:cur"* ]]; then echo NEW; fi ;;
	"system df")
		n="$(cat "${DOCKER_DF}" 2>/dev/null || echo 0)"; echo $((n + 1)) > "${DOCKER_DF}"
		if ((n == 0)); then printf '%s\n' 3.5GB 0B 120MB 2GB; else printf '%s\n' 2.5GB 0B 120MB 2GB; fi ;;
esac
exit 0
SHIM
chmod +x "${scratch}/bin/docker"
export PATH="${scratch}/bin:${PATH}" DOCKER_LOG="${scratch}/docker.log" DOCKER_DF="${scratch}/df"

fTidy(){   ## fTidy PASSED KEEP REBUILT [VAR=VALUE...]
	local passed="$1" keep="$2" rebuilt="$3"; shift 3
	: > "${DOCKER_LOG}"; rm -f "${DOCKER_DF}" "${scratch}/work/.docker-built"
	export FAKE_CTRS="c1 c2" FAKE_DANGLING="D1" FAKE_INUSE=""
	local kv; for kv; do export "${kv?}"; done
	if [[ "${rebuilt}" != none ]]; then printf 'repo\tcur\tOLD\t%s\n' "${rebuilt}" > "${scratch}/work/.docker-built"; fi
	fDocker_Tidy "${scratch}/work" "${passed}" "${keep}"
}
fRmi(){ grep '^rmi ' "${DOCKER_LOG}" | sort | tr '\n' ' '; }

## A cached build keeps its layers, and only a real rebuild prunes.
rm -f "${scratch}/work/.docker-built"
fDocker_Built "${scratch}/work" repo cur OLD '["L1"]'
fDocker_Built "${scratch}/work" repo cur OLD '["L0"]'
[[ "$(cut -f4 "${scratch}/work/.docker-built" | tr '\n' ' ')" == "0 1 " ]] || fFail "rebuilds noted: $(cat "${scratch}/work/.docker-built")"

fTidy 1 0 1
grep -qx 'stop --time 5 c1 c2' "${DOCKER_LOG}" || fFail "the run's containers weren't stopped: $(cat "${DOCKER_LOG}")"
grep -qx 'rm c1 c2' "${DOCKER_LOG}" || fFail "the run's containers weren't removed"
[[ "$(fRmi)" == "rmi D1 rmi O1 rmi O2 rmi OLD " ]] || fFail "after a pass, removed: $(fRmi)"
grep -qx 'builder prune --force --filter until=168h' "${DOCKER_LOG}" || fFail "no week-old build cache prune after a rebuild"
[[ "${dockerNote}" == "2 container(s), 4 old image(s), build cache over a week old; docker freed about 1.0 GB" ]] || fFail "note: ${dockerNote}"

fTidy 0 0 0
[[ "$(fRmi)" == "rmi D1 rmi OLD " ]] || fFail "after a failure, removed: $(fRmi)"
if grep -q '^builder' "${DOCKER_LOG}"; then fFail "pruned build cache with no rebuild"; fi

fTidy 1 1 0
grep -qx 'stop --time 5 c1 c2' "${DOCKER_LOG}" || fFail "--keep-temp left the containers running"
if grep -q '^rm ' "${DOCKER_LOG}"; then fFail "--keep-temp removed the containers"; fi
[[ "${dockerNote}" == "kept 2 stopped container(s), labeled dev.run=work, 4 old image(s);"* ]] || fFail "--keep-temp note: ${dockerNote}"

fTidy 1 0 0 FAKE_INUSE=O1
[[ "$(fRmi)" == "rmi D1 rmi O2 rmi OLD " ]] || fFail "with O1 in use, removed: $(fRmi)"

fTidy 1 0 none FAKE_CTRS= FAKE_DANGLING=
[[ -z "${dockerNote}" ]] || fFail "nothing to do, yet: ${dockerNote}"
if grep -q '^system df' "${DOCKER_LOG}"; then fFail "measured with nothing to do"; fi

if grep -qE '^(rmi -f|rm -f|system prune|image prune|container prune|volume)' "${DOCKER_LOG}"; then fFail "a call that reaches past the project: $(cat "${DOCKER_LOG}")"; fi

## Kept cache folders.
cache="${scratch}/tree/cicd/cache/home"
mkdir -p "${cache}"; echo x > "${cache}/f"
fCache_Fit "${cache}" one > /dev/null
[[ -f "${cache}/f" && "$(cat "${cache}/.shclwrap-stamp")" == one ]] || fFail "a first look didn't stamp and keep the cache"
fCache_Fit "${cache}" two > "${scratch}/out"
[[ ! -e "${cache}/f" && "$(cat "${cache}/.shclwrap-stamp")" == two ]] || fFail "another toolchain's cache was kept"
grep -q '^Emptied cicd/cache/home (.*filled by one)$' "${scratch}/out" || fFail "no line for the emptied cache: $(cat "${scratch}/out")"
echo x > "${cache}/f"
cacheCap=0 fCache_Fit "${cache}" two > /dev/null
[[ ! -e "${cache}/f" ]] || fFail "a cache over the cap was kept"
mkdir -p "${scratch}/elsewhere/keep"; ln -s "${scratch}/elsewhere/keep" "${scratch}/tree/cicd/cache/link"
if fCache_Rm "${scratch}/elsewhere/keep" 2>/dev/null; then fFail "removed a folder outside cicd/cache"; fi
if fCache_Rm "${scratch}/tree/cicd/cache/link" 2>/dev/null; then fFail "removed through a link"; fi
[[ -d "${scratch}/elsewhere/keep" ]] || fFail "the link's target went"

## cicd's own docker calls: every build labeled, every run --rm by flag, every prune filtered.
mapfile -t scripts < <(find "${here}" \( -path "${here}/cache" -o -path "${here}/artifacts" \) -prune -o -type f -name '*.bash' ! -name "${0##*/}" -print)
for script in "${scripts[@]}"; do
	runs="$(grep -cE '^[^#]*docker[[:space:]]run ' "${script}" || true)"
	builds="$(grep -cE '^[^#]*docker[[:space:]]build ' "${script}" || true)"
	labels="$(grep -cF -- '--label dev.project=shclwrap --label "dev.run=${runId}"' "${script}" || true)"
	labels=$((labels - runs))
	((builds == 0 || labels >= builds)) || fFail "${script##*/}: ${builds} image build(s), ${labels} labeled"
	rmByFlag="$(grep -cF -- ' run "${rmFlag[@]}" --init ' "${script}" || true)"
	((runs == rmByFlag)) || fFail "${script##*/}: ${runs} container run(s), ${rmByFlag} with --rm by --keep-temp"
	if grep -nE '^[^#]*docker[[:space:]]+(builder[[:space:]]+prune|system[[:space:]]+prune|rmi[[:space:]]+-f)' "${script}" | grep -v 'until=168h'; then
		fFail "${script##*/}: an unfiltered prune or a forced rmi"
	fi
done
grep -qE '^[[:space:]]*fDocker_Tidy "\$\{work\}" "\$\{runPassed\}" "\$\{keepTemp\}"' "${here}/cicd.bash" || fFail "cicd.bash's end of run doesn't tidy docker"

echo "PASS dedqv a run's end removes its containers and stale images, and nothing else"


##	History:
##		- 2026-10-25 JC: Created.
