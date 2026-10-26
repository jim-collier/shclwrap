#!/usr/bin/env bash

#  shellcheck disable=1091  ## 'Not following.' The includes are found at run time.
#  shellcheck disable=2154  ## 'referenced but not assigned.' cpuJobs comes from cpu-cap.bash.

##	Purpose: Builds each language's ops driver the way it ships, and runs the
##		shared cases through the built program: Go and Rust release builds, C
##		without sanitizers, and Python's. The smoke check on GitHub's Macs, after
##		cicd.bash has fetched the pinned shcl.h and shcl.py (2026102307090301).
##	Syntax: driver-smoke.bash OUTDIR
##	Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT

set -Eeuo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${here}/../.." && pwd)"
source "${root}/source/c/shcl-pins.env"
source "${root}/source/python/shcl-pins.env"
source "${here}/include/cpu-cap.bash"
fLowPriority
fCpuCap

out="${1:?usage: driver-smoke.bash OUTDIR}"
mkdir -p "${out}"
export PATH="${HOME}/.cargo/bin:${PATH}" PYTHONPYCACHEPREFIX="${out}/pycache"
cases=(python3 "${root}/cicd/c_cases.py" --version 3 --cases "${root}/tests/cases")
shcl2Dir="${root}/cicd/cache/shcl-c/${SHCL2_REV}"
shcl3Dir="${root}/cicd/cache/shcl-c/${SHCL3_REV}"
pyDir="${root}/cicd/cache/shcl-py/${SHCL3_PY_REV}"
for f in "${shcl2Dir}/shcl.h" "${shcl3Dir}/shcl.h" "${pyDir}/shcl.py"; do
	[[ -f "${f}" ]] || { echo "driver-smoke: no ${f}; run cicd/cicd.bash first, which fetches it" >&2; exit 1; }
done

(cd "${root}/source/go" && go build -p "${cpuJobs}" -trimpath -o "${out}/go-drive" ./cmd/shclw-drive)
"${cases[@]}" --lang Go --drive "${out}/go-drive"

## The same build lang-checks.bash makes for the fuzz, so it's usually there already.
(cd "${root}/source/rust" && cargo build --quiet --locked -j "${cpuJobs}" --release --all-features --bin shclw-drive)
"${cases[@]}" --lang Rust --drive "${CARGO_TARGET_DIR:-${root}/source/rust/target}/release/shclw-drive"

make -s -C "${root}/source/c" -j "${cpuJobs}" SHCL_DIR="${shcl3Dir}" SHCL2_DIR="${shcl2Dir}" OUT="${out}/c" \
	CC="${CC15:-gcc-15}" CC15="${CC15:-gcc-15}" drive-plain
"${cases[@]}" --drive "${out}/c/shclw-drive-plain"

PYTHONPATH="${pyDir}" "${cases[@]}" --lang Python --drive python3 "${root}/source/python/shclw_drive.py"


##	History:
##		- 2026-10-25 JC: Created.
