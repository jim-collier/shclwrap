#!/usr/bin/env bash

#  shellcheck disable=1091  ## 'Not following.' The includes are found at run time.
#  shellcheck disable=2086  ## FLAGS is split into words on purpose.

##	Purpose: Test defmn. Which cicd.bash runs start the ci workflow on GitHub,
##		and how the start and result steps read GitHub's answers, against a
##		scratch repo pushed to a local bare remote, with gh stood in for
##		(2026102519333340).
##	Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
##	SPDX-License-Identifier: MIT

set -Eeuo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${here}/utility/include/safe-rm.bash"
fails=0
fFail(){ echo "defmn: $*" >&2; fails=$((fails + 1)); }

## Start first after the sync, and result last, in a full run. Off by name in a quick one.
fPlan(){
	local flags="$1" want="$2" plan steps
	plan="$(env -u SHCLW_FUZZ_SECONDS -u SHCLW_GOFUZZ_SECONDS bash "${here}/cicd.bash" --dry-run ${flags})"
	steps="$(sed -n 's/^Steps \.*: [0-9]*: //p' <<<"${plan}")"
	steps="${steps#Remote sync, }"
	if [[ "${want}" == on ]]; then
		[[ "${steps}" == "GitHub ci: start, "*", GitHub ci: result" ]] || fFail "'${flags}': steps are '${steps}'"
	else
		[[ "${steps}" != *GitHub* ]] || fFail "'${flags}' starts it"
		grep -qxF "                GitHub ci: start (${want})" <<<"${plan}" || grep -qxF "Turned off ...: GitHub ci: start (${want})" <<<"${plan}" \
			|| fFail "'${flags}' doesn't say it's off for ${want}"
	fi
	return 0
}
fPlan ""                         on
fPlan "--merge-gate"             on
fPlan "--backlog-run-full"       on
fPlan "--ci --no-container"      on
fPlan "--quick"                  --quick
fPlan "--quick --leakcheck"      --quick
fPlan "--backlog-run-quick cicd" --backlog-run-quick
fPlan "--no-remote"              --no-remote
fPlan "--merge-gate --no-remote" --no-remote
grep -qE '^exec .*cicd\.bash" --merge-gate .*--no-remote' "${here}/hooks/pre-push" || fFail "the pre-push hook would wait on GitHub"
## A runner has GITHUB_ACTIONS too, but --no-remote says it where it's read.
jobs="$(grep -cE 'run: bash cicd/cicd\.bash ' "${here}/../.github/workflows/ci.yml" || true)"
[[ "${jobs}" -gt 0 && "${jobs}" == "$(grep -cE 'run: bash cicd/cicd\.bash .*--no-remote' "${here}/../.github/workflows/ci.yml" || true)" ]] || fFail "a ci.yml job runs cicd.bash without --no-remote"

scratch="$(mktemp -d "${TMPDIR:-/tmp}/test_shclw_hosted_XXXXXX")"
touch "${scratch}/.shclw-hosted-owned"
fCleanup(){
	if [[ -f "${scratch}/.shclw-hosted-owned" ]]; then fSafeRm "${scratch}" >/dev/null; fi
	return 0
}
trap fCleanup EXIT

mkdir -p "${scratch}/bin"
cat > "${scratch}/bin/gh" <<'EOF_gh'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "${HOSTED_GH_LOG:?}"
case "${1:-} ${2:-}" in
	"repo view")    printf '%s\n' "${HOSTED_GH_VIS:-PUBLIC}" ;;
	"workflow run") exit "${HOSTED_GH_DISPATCH:-0}" ;;
	"run list")     printf '%s\n' "${HOSTED_GH_RUN:-}" ;;
	"run view")     printf '%s|%s|https://example.invalid/run/7\n' "${HOSTED_GH_STATUS:-completed}" "${HOSTED_GH_CONCLUSION:-}" ;;
	*) exit 1 ;;
esac
EOF_gh
chmod +x "${scratch}/bin/gh"
fGit(){ git -C "${scratch}/repo" -c core.hooksPath=/dev/null -c user.name=t -c user.email=t@t "$@"; }
git init -q --bare "${scratch}/remote.git"
git init -q -b round "${scratch}/repo"
mkdir -p "${scratch}/repo/.github/workflows"
printf 'name: ci\n' > "${scratch}/repo/.github/workflows/ci.yml"
fGit add .github/workflows/ci.yml
fGit commit -q -m first
fGit remote add origin "${scratch}/remote.git"
fGit push -q -u origin round 2>/dev/null

export SHCLW_GH="${scratch}/bin/gh" HOSTED_GH_LOG="${scratch}/gh.log"
## fHosted ENV...: the start's lines, then the result's, then "rc N".
fHosted(){
	(
		source "${here}/utility/include/steps.bash"
		source "${here}/utility/include/hosted.bash"
		# shellcheck disable=2034  ## Read by hosted.bash.
		{ hostedRoot="${scratch}/repo"; hostedCapSecs=60; }
		# shellcheck disable=2329  ## Called by hosted.bash.
		sleep(){ :; }
		declare -x GITHUB_ACTIONS="" "$@"
		: > "${HOSTED_GH_LOG}"
		fHosted_Start
		rc=0; fHosted_Result || rc=$?
		echo "rc ${rc}"
	) 2>&1
}

out="$(fHosted HOSTED_GH_RUN=7 HOSTED_GH_CONCLUSION=success)"
{ grep -qx 'PASS defmk ci workflow started on GitHub' <<<"${out}" && grep -qx 'PASS defmm ci workflow passed on GitHub' <<<"${out}" && grep -qx 'rc 0' <<<"${out}"; } || fFail "passed: '${out//$'\n'/ | }'"
grep -qx 'workflow run ci.yml --ref round' "${HOSTED_GH_LOG}" || fFail "passed: not asked for on the branch"
grep -qE "^run list .*--branch round --event workflow_dispatch --commit $(fGit rev-parse HEAD) .*createdAt >= \"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z\"" "${HOSTED_GH_LOG}" \
	|| fFail "passed: the run looked for isn't HEAD's dispatch since the start"

out="$(fHosted HOSTED_GH_RUN=7 HOSTED_GH_CONCLUSION=failure)"
{ grep -qx 'FAIL defmm ci workflow passed on GitHub' <<<"${out}" && grep -qx 'rc 1' <<<"${out}"; } || fFail "failed: '${out//$'\n'/ | }'"

## Short of a finished run that failed, it's a skip.
out="$(fHosted HOSTED_GH_RUN=7 HOSTED_GH_CONCLUSION=cancelled)"
{ grep -q '^SKIP defmm .*, cancelled on GitHub' <<<"${out}" && grep -qx 'rc 0' <<<"${out}"; } || fFail "cancelled: '${out//$'\n'/ | }'"
out="$(fHosted HOSTED_GH_RUN="")"
{ grep -q '^SKIP defmm .*, GitHub listed no run' <<<"${out}" && grep -qx 'rc 0' <<<"${out}"; } || fFail "unlisted: '${out//$'\n'/ | }'"
out="$(fHosted HOSTED_GH_RUN=7 HOSTED_GH_STATUS=in_progress)"
{ grep -q "^SKIP defmm .*, still 'in_progress' after 1 minutes" <<<"${out}" && grep -qx 'rc 0' <<<"${out}"; } || fFail "timed out: '${out//$'\n'/ | }'"
out="$(fHosted HOSTED_GH_DISPATCH=1)"
{ [[ "$(grep -c "^SKIP defm[km] .*, gh couldn't start it" <<<"${out}")" == 2 ]] && grep -qx 'rc 0' <<<"${out}"; } || fFail "refused: '${out//$'\n'/ | }'"

## These never ask GitHub for a run.
fNever(){   ## fNever WHAT WANT ENV...
	local what="$1" want="$2"
	shift 2
	out="$(fHosted "$@")"
	{ grep -qE "^SKIP defmk .*, ${want}" <<<"${out}" && grep -q '^SKIP defmm ' <<<"${out}" && grep -qx 'rc 0' <<<"${out}"; } || fFail "${what}: '${out//$'\n'/ | }'"
	if grep -q '^workflow run' "${HOSTED_GH_LOG}"; then fFail "${what}: asked for a run"; fi
	return 0
}
fNever runner  'already on a GitHub runner$' GITHUB_ACTIONS=true
fNever private 'the repo is private, and the Mac runner is free only on a public one$' HOSTED_GH_VIS=PRIVATE
fGit commit -q --allow-empty -m second
fNever unpushed "origin/round isn't at HEAD [0-9a-f]{12}, so push first$"
fGit checkout -q --detach
fNever detached 'HEAD is detached'
fGit checkout -q round
fGit push -q origin round 2>/dev/null
fGit rm -q .github/workflows/ci.yml
fGit commit -q -m third
fGit push -q origin round 2>/dev/null
fNever 'no workflow' 'no .github/workflows/ci.yml$'

if ((fails)); then echo "defmn: ${fails} check(s) failed" >&2; exit 1; fi
echo "PASS defmn the ci workflow on GitHub: which runs start it, and how its result reads"


##	History:
##		- 2026-10-26 JC: Created.
