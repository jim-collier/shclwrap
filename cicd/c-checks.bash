#!/usr/bin/env bash
# shellcheck disable=1091
# shellcheck disable=2154  ## cpuJobs is set by cpu-cap.bash.

##	Purpose: Runs one C mode of c-in-container.bash in the pinned C image, or on
##		the host with SHCLW_C_HOST=1 (cicd.bash --no-container), when docker is
##		missing, or when the image won't build. The host run says so in one line,
##		and skips what it has no tool for.

set -Eeuo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${here}/.." && pwd)"
source "${here}/tool-pins.env"
source "${here}/utility/include/run-lock.bash"
source "${here}/utility/include/cpu-cap.bash"
source "${here}/utility/include/docker-tidy.bash"
fLowPriority

if (($# < 3)); then
	echo "usage: c-checks.bash MODE VERSION WORKDIR [FUZZ ARGS...]" >&2
	exit 2
fi

mode="$1"
version="$2"
work="$(realpath "$3")"
## Its own lock when run alone; under cicd.bash this passes straight through.
fRunLock "${root}" "c-checks.bash $*"
## Checked here, not taken from cicd.bash, since the load may have moved since its stage began.
case "${mode}" in
	test|fuzz|leakcheck|leakcheck-resume) fCpuCap hard ;;
	*) fCpuCap ;;
esac
artifacts="${root}/cicd/artifacts/leakcheck"
## Kept between runs, so CodeQL's compiled queries are too (about 40 min cold).
home="${root}/cicd/cache/c-home"
mkdir -p "${artifacts}" "${home}"
repo="shclwrap-c-tools"
tag="${repo}:${PIN_GCC15_PACKAGE}_${PIN_CLANG_PACKAGE//:/_}"

## fOnHost WHY: the same mode, run here. A missing docker never fails a run.
fOnHost(){
	echo "WARNING: C runs on the host: $1"
	exec env SHCLW_C_HOST=1 SHCLW_C_WORK="${work}" SHCLW_C_ARTIFACTS="${artifacts}" \
		bash "${here}/c-in-container.bash" "${mode}" "${version}" "${@:2}"
}
if [[ "${SHCLW_C_HOST:-}" == 1 ]]; then fOnHost "--no-container" "${@:4}"; fi
if ! command -v docker >/dev/null || ! docker version --format '{{.Server.Version}}' >/dev/null 2>&1; then
	fOnHost "no docker, or its daemon isn't running" "${@:4}"
fi

## Named and labeled by the run's scratch folder, so a run's containers and images can be found.
runId="$(fDocker_RunId "${work}")"
oldId="$(docker image inspect --format '{{.Id}}' "${tag}" 2>/dev/null || true)"
oldLayers="$(fDocker_Layers "${tag}")"
buildLog="${root}/cicd/artifacts/c-image-build.log"
docker build --quiet --file "${here}/C.Dockerfile" \
	--build-arg "BASE_IMAGE=${PIN_CTOOLS_IMAGE}" \
	--build-arg "GCC15_PACKAGE=${PIN_GCC15_PACKAGE}" \
	--build-arg "CLANG_PACKAGE=${PIN_CLANG_PACKAGE}" \
	--build-arg "HEAPTRACK_PACKAGE=${PIN_HEAPTRACK_PACKAGE}" \
	--build-arg "MINGW_PACKAGE=${PIN_MINGW_PACKAGE}" \
	--build-arg "MINGW_HEADERS_PACKAGE=${PIN_MINGW_HEADERS_PACKAGE}" \
	--label dev.project=shclwrap --label "dev.run=${runId}" \
	--tag "${tag}" "${here}" >"${buildLog}" 2>&1 \
	|| fOnHost "the C image didn't build; see cicd/artifacts/${buildLog##*/}" "${@:4}"
## The old image, older tags and build cache go at the run's end, in cicd.bash.
fDocker_Built "${work}" "${repo}" "${tag##*:}" "${oldId}" "${oldLayers}"
## CodeQL's compiled queries are most of it.
fCache_Fit "${home}" "codeql ${PIN_CODEQL} ${PIN_CODEQL_QUERY_COMMIT}"

network=none
if [[ "${mode}" == leakcheck ]]; then network=bridge; fi

name="shclwrap-${runId}-${mode}"
## cicd.bash --keep-temp keeps the run's containers, stopped, for a look.
rmFlag=(--rm); if [[ "${SHCLW_KEEP_TEMP:-}" == 1 ]]; then rmFlag=(); fi
## A WORK name another tree also uses, run alone.
if docker container inspect "${name}" >/dev/null 2>&1; then name+="-$$"; fi

## The init passes Ctrl+C on to the whole tree, so an aborted run doesn't leave the container going.
## nice on this side doesn't reach the container, so it gets a CPU cap, a low share, and its own nice.
exec docker run "${rmFlag[@]}" --init --env TINI_KILL_PROCESS_GROUP=1 --user "$(id -u):$(id -g)" --network="${network}" \
	--name "${name}" --label dev.project=shclwrap --label "dev.run=${runId}" \
	--cpus "${cpuJobs}" --cpu-shares 256 --env SHCLW_JOBS="${cpuJobs}" --env GOMAXPROCS="${cpuJobs}" \
	--env HOME=/home/ci \
	--mount "type=bind,src=${home},dst=/home/ci" \
	--mount "type=bind,src=${root},dst=/src,readonly" \
	--mount "type=bind,src=${work},dst=/work" \
	--mount "type=bind,src=${artifacts},dst=/artifacts" \
	--workdir /src \
	"${tag}" nice -n 19 bash /src/cicd/c-in-container.bash "${mode}" "${version}" "${@:4}"
