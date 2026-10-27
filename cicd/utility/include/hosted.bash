#!/usr/bin/env bash

##	Purpose: The ci workflow on GitHub, for cicd.bash (backlog 2026102519333340).
##		Source this after steps.bash, set hostedRoot to the repo, and call:
##			fHosted_Start    asks GitHub to run ci.yml on the branch, at HEAD
##			fHosted_Result   waits for that run; returns 1 only when it finished and failed
##		GitHub runs only what's pushed, and dispatch works only once the workflow is
##		on the default branch. Like a test box that doesn't answer, anything short
##		of a finished run that failed is a SKIP. SHCLW_GH stands in for gh.
##	Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT

hostedRoot="${hostedRoot:-}"; hostedWorkflow="ci.yml"
hostedSha=""; hostedBranch=""; hostedSince=""; hostedSkipWhy=""
## Over both jobs' timeouts, plus a wait for a runner.
declare -i hostedCapSecs=6600 hostedPollSecs=30

## gh finds the repo from the folder it runs in, and gitsby the account.
fHosted_Gh(){
	(
		cd "${hostedRoot:?}" || exit 1
		if   [[ -n "${SHCLW_GH:-}" ]];          then "${SHCLW_GH}" "$@"
		elif command -v gitsby >/dev/null 2>&1; then gitsby -q raw gh "$@"
		else gh "$@"
		fi
	)
}

fHosted_SkipStart(){  fTest_Skip defmk "ci workflow started on GitHub" "$1"; }
fHosted_SkipResult(){ fTest_Skip defmm "ci workflow passed on GitHub" "$1"; }

fHosted_Start(){
	local branch="" remote="" remoteSha="" seen=""
	hostedSkipWhy=""; hostedBranch=""
	hostedSha="$(git -C "${hostedRoot:?}" rev-parse HEAD)"
	branch="$(git -C "${hostedRoot}" symbolic-ref --short -q HEAD || true)"
	if   [[ "${GITHUB_ACTIONS:-}" == "true" ]]; then hostedSkipWhy="already on a GitHub runner"
	elif [[ ! -f "${hostedRoot}/.github/workflows/${hostedWorkflow}" ]]; then hostedSkipWhy="no .github/workflows/${hostedWorkflow}"
	elif [[ -z "${SHCLW_GH:-}" ]] && ! command -v gh >/dev/null 2>&1; then hostedSkipWhy="no gh"
	elif [[ -z "${branch}" ]]; then hostedSkipWhy="HEAD is detached, so there's no branch to run it on"
	elif ! remote="$(git -C "${hostedRoot}" config "branch.${branch}.remote")"; then hostedSkipWhy="${branch} tracks no remote"
	elif ! remoteSha="$(git -C "${hostedRoot}" ls-remote "${remote}" "refs/heads/${branch}")"; then hostedSkipWhy="couldn't reach ${remote}"
	elif [[ "${remoteSha%%[[:space:]]*}" != "${hostedSha}" ]]; then hostedSkipWhy="${remote}/${branch} isn't at HEAD ${hostedSha:0:12}, so push first"
	## The Mac runner is paid on a private repo.
	elif ! seen="$(fHosted_Gh repo view --json visibility --jq .visibility)"; then hostedSkipWhy="gh couldn't read the repo's visibility"
	elif [[ "${seen}" != "PUBLIC" ]]; then hostedSkipWhy="the repo is ${seen,,}, and the Mac runner is free only on a public one"
	fi
	if [[ -n "${hostedSkipWhy}" ]]; then fHosted_SkipStart "${hostedSkipWhy}"; return 0; fi
	## A minute early, for a clock that runs ahead of GitHub's.
	TZ=UTC printf -v hostedSince '%(%Y-%m-%dT%H:%M:%SZ)T' "$((EPOCHSECONDS - 60))"
	if ! fHosted_Gh workflow run "${hostedWorkflow}" --ref "${branch}" >/dev/null; then
		hostedSkipWhy="gh couldn't start it, and said why above"
		fHosted_SkipStart "${hostedSkipWhy}"; return 0
	fi
	hostedBranch="${branch}"
	fEcho_Clean "Hosted .......: ${hostedWorkflow} on ${remote}/${branch} at ${hostedSha:0:12}"
	if [[ -n "$(git -C "${hostedRoot}" status --porcelain --untracked-files=no)" ]]; then
		fEcho_Clean "Note .........: it runs the commit, not the uncommitted edits here"
	fi
	fTest_Start defmk "ci workflow started on GitHub"; fTest_Pass
}

fHosted_Result(){
	if [[ -z "${hostedBranch}" ]]; then fHosted_SkipResult "${hostedSkipWhy:-not started}"; return 0; fi
	local runId="" state="" status="" conclusion="" url=""
	local -i waited=0
	## GitHub lists a dispatched run some seconds after it's asked for.
	while :; do
		runId="$(fHosted_Gh run list --workflow "${hostedWorkflow}" --branch "${hostedBranch}" --event workflow_dispatch --commit "${hostedSha}" \
			--json databaseId,createdAt --jq "map(select(.createdAt >= \"${hostedSince}\")) | .[0].databaseId // empty")" || runId=""
		[[ -z "${runId}" ]] || break
		if ((waited >= 120)); then fHosted_SkipResult "GitHub listed no run of it 2 minutes after it was started"; return 0; fi
		sleep 10; waited+=10
	done
	waited=0
	while :; do
		state="$(fHosted_Gh run view "${runId}" --json status,conclusion,url --jq '.status + "|" + .conclusion + "|" + .url')" || state=""
		IFS='|' read -r status conclusion url <<< "${state}" || true
		[[ "${status}" != "completed" ]] || break
		if ((waited == 0)); then fEcho_Clean "Waiting ......: on ${url:-run ${runId}}"; fi
		if ((waited >= hostedCapSecs)); then fHosted_SkipResult "still '${status:-unknown}' after $((hostedCapSecs / 60)) minutes: ${url:-run ${runId}}"; return 0; fi
		sleep "${hostedPollSecs}"; waited+=hostedPollSecs
	done
	fEcho_Clean "Run ..........: ${url}"
	## ci.yml cancels a run on the branch when a newer one starts.
	if [[ "${conclusion}" == "cancelled" ]]; then fHosted_SkipResult "cancelled on GitHub: ${url}"; return 0; fi
	fTest_Start defmm "ci workflow passed on GitHub"
	if [[ "${conclusion}" != "success" ]]; then
		echo "${hostedWorkflow} ended '${conclusion}' on GitHub: ${url}" >&2
		fTest_Fail; return 1
	fi
	fTest_Pass
}


##	History:
##		- 2026-10-26 JC: Created, from zuid's hosted stages. A private repo skips it, since the Mac runner costs there.
