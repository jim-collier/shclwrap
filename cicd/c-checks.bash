#!/usr/bin/env bash
# shellcheck disable=1091

set -Eeuo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${here}/.." && pwd)"
source "${here}/tool-pins.env"

if (($# < 3)); then
	echo "usage: c-checks.bash MODE VERSION WORKDIR [FUZZ ARGS...]" >&2
	exit 2
fi

mode="$1"
version="$2"
work="$(realpath "$3")"
artifacts="${root}/cicd/artifacts/leakcheck"
## Kept between runs, so CodeQL's compiled queries are too (about 40 min cold).
home="${root}/cicd/cache/c-home"
mkdir -p "${artifacts}" "${home}"
repo="shclwrap-c-tools"
tag="${repo}:${PIN_GCC15_PACKAGE}_${PIN_CLANG_PACKAGE//:/_}"

fImageLayers(){ docker image inspect --format '{{json .RootFS.Layers}}' "$1" 2>/dev/null || true; }

oldId="$(docker image inspect --format '{{.Id}}' "${tag}" 2>/dev/null || true)"
oldLayers="$(fImageLayers "${tag}")"
docker build --quiet --file "${here}/C.Dockerfile" \
	--build-arg "BASE_IMAGE=${PIN_CTOOLS_IMAGE}" \
	--build-arg "GCC15_PACKAGE=${PIN_GCC15_PACKAGE}" \
	--build-arg "CLANG_PACKAGE=${PIN_CLANG_PACKAGE}" \
	--build-arg "HEAPTRACK_PACKAGE=${PIN_HEAPTRACK_PACKAGE}" \
	--build-arg "MINGW_PACKAGE=${PIN_MINGW_PACKAGE}" \
	--build-arg "MINGW_HEADERS_PACKAGE=${PIN_MINGW_HEADERS_PACKAGE}" \
	--tag "${tag}" "${here}"
newId="$(docker image inspect --format '{{.Id}}' "${tag}")"

## Drop the image this build replaced and any other pin's, unless a container still uses one.
mapfile -t staleIds < <({ if [[ -n "${oldId}" ]]; then echo "${oldId}"; fi; docker image ls --no-trunc --quiet "${repo}"; } | sort -u)
for id in "${staleIds[@]}"; do
	[[ "${id}" != "${newId}" ]] || continue
	docker image inspect "${id}" >/dev/null 2>&1 || continue
	[[ -z "$(docker ps --all --quiet --filter "ancestor=${id}")" ]] || continue
	if docker rmi "${id}" >/dev/null; then echo "Removed the old C tools image ${id:7:12}"; fi
done
## The image ID moves on every build, even a cached one, so a real rebuild shows as new layers.
if [[ "$(fImageLayers "${tag}")" != "${oldLayers}" ]]; then docker builder prune --force >/dev/null || true; fi

network=none
if [[ "${mode}" == leakcheck ]]; then network=bridge; fi

## The init passes Ctrl+C on to the whole tree, so an aborted run doesn't leave the container going.
exec docker run --rm --init --env TINI_KILL_PROCESS_GROUP=1 --user "$(id -u):$(id -g)" --network="${network}" \
	--env HOME=/home/ci \
	--mount "type=bind,src=${home},dst=/home/ci" \
	--mount "type=bind,src=${root},dst=/src,readonly" \
	--mount "type=bind,src=${work},dst=/work" \
	--mount "type=bind,src=${artifacts},dst=/artifacts" \
	--workdir /src \
	"${tag}" bash /src/cicd/c-in-container.bash "${mode}" "${version}" "${@:4}"
