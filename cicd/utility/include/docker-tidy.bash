#!/usr/bin/env bash

#  shellcheck disable=1091  ## 'Not following.' The include is found at run time.
#  shellcheck disable=2034  ## dockerNote is for the caller.

##	Purpose: Docker upkeep for a cicd run, per the container bullets of Cleanup
##		after a run in the shared CICD rules (backlog 2026102416400020). Source
##		this and call:
##			fDocker_RunId WORK      the run's id, its scratch folder's name, which
##			                        is each container's and image's dev.run label
##			fDocker_Built WORK REPO TAG OLDID OLDLAYERS
##			                        notes an image a check script just built, for
##			                        the run's end
##			fCache_Fit DIR STAMP    empties a cache folder kept between runs when
##			                        it's over 2 GB, or when another toolchain
##			                        (STAMP) filled it
##			fDocker_Tidy WORK PASSED KEEP
##			                        the run's end. Stops and removes the run's
##			                        containers (keeps them stopped under KEEP),
##			                        removes the project's dangling images, and
##			                        after a pass the older tags, keeping the
##			                        current and the one before it. Sets dockerNote
##			                        to what went and about what it freed.
##		Only what carries dev.project=shclwrap or a shclwrap-* image name is
##		touched. Never rmi -f, and never docker system prune.
##	Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
##	SPDX-License-Identifier: MIT

source "$(dirname "${BASH_SOURCE[0]}")/safe-rm.bash"

cacheCap=$((2 * 1000 * 1000))   ## KB, so 2 GB

fDocker_RunId(){ local r="${1##*/}"; printf '%s' "${r//[^A-Za-z0-9_.-]/_}"; }
fDocker_Layers(){ docker image inspect --format '{{json .RootFS.Layers}}' "$1" 2>/dev/null || true; }
fDocker_Up(){ command -v docker >/dev/null && docker version --format '{{.Server.Version}}' >/dev/null 2>&1; }

fDocker_Built(){   ## fDocker_Built WORK REPO TAG OLDID OLDLAYERS
	local rebuilt=0
	## The image ID moves on every build, even a cached one, so a real rebuild shows as new layers.
	if [[ "$(fDocker_Layers "$2:$3")" != "$5" ]]; then rebuilt=1; fi
	printf '%s\t%s\t%s\t%s\n' "$2" "$3" "${4:-none}" "${rebuilt}" >> "$1/.docker-built"
}

## Sizes from docker system df, summed, in bytes. Shared layers make per-image sizes overlap.
fDocker_Df(){
	docker system df --format '{{.Size}}' 2>/dev/null | awk '
		{ n = $1 + 0; u = $1; sub(/^[0-9.]+/, "", u); m = 1
		  if (u == "kB") m = 1e3; else if (u == "MB") m = 1e6; else if (u == "GB") m = 1e9; else if (u == "TB") m = 1e12
		  t += n * m }
		END { printf "%.0f\n", t }'
}
fBytes(){ awk -v b="$1" 'BEGIN { split("B kB MB GB TB", u, " "); i = 1; while (b >= 1000 && i < 5) { b /= 1000; i++ }; printf((i == 1) ? "%d %s" : "%.1f %s", b, u[i]) }'; }

## Only a folder right under a tree's cicd/cache/, never a link or a path through one.
fCache_Rm(){
	local p="${1%/}"
	if ! [[ "${p}" =~ ^/.+/cicd/cache/[A-Za-z0-9_.-]+$ ]]; then echo "cache-rm: refused: ${p}" >&2; return 1; fi
	fSafeRm_Under "${p%/*}" "${p}" >/dev/null
}

fCache_Fit(){   ## fCache_Fit DIR STAMP
	local dir="$1" stamp="$2" had="" size=0 why=""
	mkdir -p "${dir}"
	had="$(cat "${dir}/.shclwrap-stamp" 2>/dev/null || true)"
	size="$(du -sk "${dir}" 2>/dev/null | cut -f1 || true)"
	if [[ -n "${had}" && "${had}" != "${stamp}" ]]; then why="filled by ${had}"
	elif ((size > cacheCap)); then why="over 2 GB"
	fi
	if [[ -n "${why}" ]]; then
		fCache_Rm "${dir}"
		mkdir "${dir}"
		echo "Emptied cicd/cache/${dir##*/} ($(fBytes $((size * 1000))), ${why})"
	fi
	printf '%s\n' "${stamp}" > "${dir}/.shclwrap-stamp"
}

fDocker_Tidy(){   ## fDocker_Tidy WORK PASSED KEEP
	local work="$1" passed="$2" keep="$3" runId repo tag oldId rebuilt t id other prune=0 before="" after="" gone=0
	local -a ctrs=() imgs=() parts=()
	local -A seen=() current=()
	dockerNote=""
	fDocker_Up || return 0
	runId="$(fDocker_RunId "${work}")"
	mapfile -t ctrs < <(docker ps --all --quiet --filter label=dev.project=shclwrap --filter "label=dev.run=${runId}")
	mapfile -t imgs < <(docker image ls --quiet --no-trunc --filter dangling=true --filter label=dev.project=shclwrap)
	if ((${#ctrs[@]} == 0 && ${#imgs[@]} == 0)) && [[ ! -s "${work}/.docker-built" ]]; then return 0; fi
	before="$(fDocker_Df)"

	if ((${#ctrs[@]})); then
		docker stop --time 5 "${ctrs[@]}" >/dev/null 2>&1 || true
		if ((keep)); then
			parts+=("kept ${#ctrs[@]} stopped container(s), labeled dev.run=${runId}")
		else
			## A --rm container may already be on its way out.
			docker rm "${ctrs[@]}" >/dev/null 2>&1 || true
			parts+=("${#ctrs[@]} container(s)")
		fi
	fi

	if [[ -s "${work}/.docker-built" ]]; then
		while IFS=$'\t' read -r repo tag oldId rebuilt; do
			if [[ "${rebuilt}" == 1 ]]; then prune=1; fi
			id="$(docker image inspect --format '{{.Id}}' "${repo}:${tag}" 2>/dev/null || true)"
			if [[ -n "${id}" ]]; then current[${id}]=1; fi
			## The image this run's first build replaced, which may predate the labels.
			if [[ "${oldId}" != none ]]; then imgs+=("${oldId}"); fi
			if [[ "${passed}" != 1 || -n "${seen[${repo}]:-}" ]]; then continue; fi
			seen[${repo}]=1
			## Newest first. Keep the current tag and the one before it.
			other=""
			while read -r t id; do
				[[ "${t}" != "${tag}" ]] || continue
				if [[ -z "${other}" ]]; then other="${t}"; continue; fi
				imgs+=("${id}")
			done < <(docker image ls --no-trunc --format '{{.Tag}} {{.ID}}' "${repo}")
		done < "${work}/.docker-built"
	fi
	if ((${#imgs[@]})); then
		mapfile -t imgs < <(printf '%s\n' "${imgs[@]}" | sort -u)
		for id in "${imgs[@]}"; do
			[[ -n "${id}" ]] || continue
			docker image inspect "${id}" >/dev/null 2>&1 || continue
			## Still a current tag, such as a cached build that kept its ID.
			[[ -z "${current[${id}]:-}" ]] || continue
			[[ -z "$(docker ps --all --quiet --filter "ancestor=${id}")" ]] || continue
			if docker rmi "${id}" >/dev/null 2>&1; then gone=$((gone + 1)); fi
		done
	fi
	if ((gone)); then parts+=("${gone} old image(s)"); fi
	if ((prune)); then
		docker builder prune --force --filter until=168h >/dev/null 2>&1 || true
		parts+=("build cache over a week old")
	fi

	((${#parts[@]})) || return 0
	after="$(fDocker_Df)"
	dockerNote="${parts[0]}"
	for t in "${parts[@]:1}"; do dockerNote+=", ${t}"; done
	if [[ -n "${before}" && -n "${after}" ]] && ((before > after)); then
		dockerNote+="; docker freed about $(fBytes $((before - after)))"
	else
		dockerNote+="; docker freed nothing measurable"
	fi
	return 0
}


##	History:
##		- 2026-10-25 JC: Created.
