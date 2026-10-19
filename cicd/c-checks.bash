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
tag="shclwrap-c-tools:${PIN_GCC15_PACKAGE}_${PIN_CLANG_PACKAGE//:/_}"

docker build --quiet --file "${here}/C.Dockerfile" \
	--build-arg "BASE_IMAGE=${PIN_CTOOLS_IMAGE}" \
	--build-arg "GCC15_PACKAGE=${PIN_GCC15_PACKAGE}" \
	--build-arg "CLANG_PACKAGE=${PIN_CLANG_PACKAGE}" \
	--build-arg "HEAPTRACK_PACKAGE=${PIN_HEAPTRACK_PACKAGE}" \
	--tag "${tag}" "${here}"

network=none
if [[ "${mode}" == leakcheck ]]; then network=bridge; fi

exec docker run --rm --user "$(id -u):$(id -g)" --network="${network}" \
	--env HOME=/home/ci \
	--mount "type=bind,src=${home},dst=/home/ci" \
	--mount "type=bind,src=${root},dst=/src,readonly" \
	--mount "type=bind,src=${work},dst=/work" \
	--mount "type=bind,src=${artifacts},dst=/artifacts" \
	--workdir /src \
	"${tag}" bash /src/cicd/c-in-container.bash "${mode}" "${version}" "${@:4}"
