#!/usr/bin/env bash
# shellcheck disable=1091
# shellcheck disable=2154  ## cpuJobs is set by cpu-cap.bash.

##	Purpose: Runs one Go, Rust and Python mode of lang-in-container.bash in the
##		pinned language image (Lang.Dockerfile), or on the host with
##		SHCLW_LANG_HOST=1 (cicd.bash --no-container), when docker is missing, or
##		when the image won't build. The host run says so in one line.
##		Areas come from SHCLW_AREAS, as cicd.bash's --backlog-run-quick names them.
##	Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
##	SPDX-License-Identifier: MIT

set -Eeuo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${here}/.." && pwd)"
source "${here}/tool-pins.env"
source "${here}/utility/include/run-lock.bash"
source "${here}/utility/include/cpu-cap.bash"
source "${here}/utility/include/docker-tidy.bash"
fLowPriority

if (($# < 2)); then
	echo "usage: lang-checks.bash MODE WORKDIR [ARGS...]" >&2
	exit 2
fi

mode="$1"
work="$(realpath "$2")"
fRunLock "${root}" "lang-checks.bash $*"
case "${mode}" in
	fuzz|gofuzz) fCpuCap hard ;;
	*) fCpuCap ;;
esac
rustToolchain="$(sed -n 's/^channel = "\(.*\)"/\1/p' "${root}/source/rust/rust-toolchain.toml")"
## Kept between runs: Go's module and build caches, cargo's registry, and Rust's target folder.
home="${root}/cicd/cache/lang-home"
mkdir -p "${home}"
repo="shclwrap-lang-tools"
tag="${repo}:${PIN_GO}_rust${rustToolchain}_py${PIN_PYTHON_CONTAINER}"

fOnHost(){   ## fOnHost WHY ARGS...: the same mode, run here.
	echo "WARNING: Go, Rust and Python run on the host: $1"
	exec env SHCLW_LANG_HOST=1 SHCLW_LANG_WORK="${work}" bash "${here}/lang-in-container.bash" "${mode}" "${@:2}"
}
if [[ "${SHCLW_LANG_HOST:-}" == 1 ]]; then fOnHost "--no-container" "${@:3}"; fi
if ! command -v docker >/dev/null || ! docker version --format '{{.Server.Version}}' >/dev/null 2>&1; then
	fOnHost "no docker, or its daemon isn't running" "${@:3}"
fi

runId="$(fDocker_RunId "${work}")"
oldId="$(docker image inspect --format '{{.Id}}' "${tag}" 2>/dev/null || true)"
oldLayers="$(fDocker_Layers "${tag}")"
buildLog="${root}/cicd/artifacts/lang-image-build.log"
mkdir -p "${buildLog%/*}"
docker build --quiet --file "${here}/Lang.Dockerfile" \
	--build-arg "BASE_IMAGE=${PIN_CTOOLS_IMAGE}" \
	--build-arg "PYTHON_PACKAGE=${PIN_PYTHON_PACKAGE}" --build-arg "PYTHON_INTERP_PACKAGE=${PIN_PYTHON_INTERP_PACKAGE}" \
	--build-arg "GO_VERSION=${PIN_GO}" --build-arg "GO_SHA256=${PIN_GO_SHA256}" \
	--build-arg "RUSTUP_VERSION=${PIN_RUSTUP}" --build-arg "RUSTUP_SHA256=${PIN_RUSTUP_SHA256}" \
	--build-arg "RUST_TOOLCHAIN=${rustToolchain}" \
	--build-arg "RUFF_VERSION=${PIN_RUFF}" --build-arg "RUFF_SHA256=${PIN_RUFF_SHA256}" \
	--build-arg "STATICCHECK_VERSION=${PIN_STATICCHECK}" \
	--label dev.project=shclwrap --label "dev.run=${runId}" \
	--tag "${tag}" "${here}" >"${buildLog}" 2>&1 \
	|| fOnHost "the Go, Rust and Python image didn't build; see cicd/artifacts/${buildLog##*/}" "${@:3}"
## Same upkeep as c-checks.bash, at the run's end.
fDocker_Built "${work}" "${repo}" "${tag##*:}" "${oldId}" "${oldLayers}"
fCache_Fit "${home}" "${tag}"

## Only these two write into the tree: gofmt -w and cargo fmt, and Go's fuzzer's failing inputs.
srcMount=",readonly"
if [[ "${mode}" == format || "${mode}" == gofuzz ]]; then srcMount=""; fi

name="shclwrap-${runId}-lang-${mode}"
rmFlag=(--rm); if [[ "${SHCLW_KEEP_TEMP:-}" == 1 ]]; then rmFlag=(); fi
if docker container inspect "${name}" >/dev/null 2>&1; then name+="-$$"; fi

## The network is for a cold module or crate cache.
exec docker run "${rmFlag[@]}" --init --env TINI_KILL_PROCESS_GROUP=1 --user "$(id -u):$(id -g)" --network=bridge \
	--name "${name}" --label dev.project=shclwrap --label "dev.run=${runId}" \
	--cpus "${cpuJobs}" --cpu-shares 256 --env SHCLW_JOBS="${cpuJobs}" --env GOMAXPROCS="${cpuJobs}" \
	--env RAYON_NUM_THREADS="${cpuJobs}" --env RUST_TEST_THREADS="${cpuJobs}" --env CARGO_BUILD_JOBS="${cpuJobs}" \
	--env SHCLW_AREAS="${SHCLW_AREAS:-}" --env SHCLW_CARGO_LOCKED="${SHCLW_CARGO_LOCKED:-}" --env SHCLW_PY="${SHCLW_PY:-}" \
	--env HOME=/home/ci --env TMPDIR=/work --env TEMP=/work --env TMP=/work --env GOPATH=/home/ci/go --env GOCACHE=/home/ci/go-build --env GOFLAGS=-modcacherw --env GOTOOLCHAIN=local \
	--env CARGO_HOME=/home/ci/cargo --env CARGO_TARGET_DIR=/home/ci/target --env RUFF_CACHE_DIR=/home/ci/ruff \
	--mount "type=bind,src=${home},dst=/home/ci" \
	--mount "type=bind,src=${root},dst=/src${srcMount}" \
	--mount "type=bind,src=${work},dst=/work" \
	--workdir /src \
	"${tag}" nice -n 19 bash /src/cicd/lang-in-container.bash "${mode}" "${@:3}"


##	History:
##		- 2026-10-25 JC: Created.
##		- 2026-10-25 JC: The image is labeled by run, --rm goes under --keep-temp, and stale images go at the end of the cicd.bash run.
##		- 2026-10-25 JC: TMPDIR, TEMP and TMP point at the run's folder in the container too.
