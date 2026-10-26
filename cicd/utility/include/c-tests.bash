#!/usr/bin/env bash

#  shellcheck disable=2034  ## cTestName is for the caller.

##	Purpose: Names of the C container's own checks, by test ID, so a run that
##		skips them names them the same way c-in-container.bash does.
##		Source steps.bash first, then this, and call:
##			fCTest_Start ID       fTest_Start with the ID's name
##			fCTest_Skip WHY ID... a SKIP line for each
##			fCTest_List ID...     "ID NAME" for each, for --list
##	Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT

declare -A cTestName=(
	[de7zp]="analyze-shcl sees a double free planted in shcl.h"
	[deb8c]="shcl's known list fails on a new finding and on a gone one"
	[deba8]="gcc's analyzer sees a leak that -O1 drops"
	[decas]="a clang analyzer finding alone fails make analyze"
	[decbj]="C builds for Windows at the same warnings"
	[de07q]="no shcl 2.0 name gets past shclw3_v2_names.h"
	[deb9f]="CTU follows a call into shcl.h, held to the known list"
	[de2xw]="LeakSanitizer sees a planted leak"
	[de2xx]="gcc's analyzer sees a planted null dereference"
	[de2xy]="heaptrack sees planted heap growth from N to 2N"
	[de2xz]="the C driver's heap holds steady from 128 to 256 soak rounds"
)
## The analyzer probes run in lint mode only. The Windows build runs under --quick too.
cTestsAnalyze=(de7zp deb8c deba8 decas)
cTestsLint=("${cTestsAnalyze[@]}" decbj)
cTestsLeak=(deb9f de2xw de2xx de2xy de2xz)
## The Makefile's names check, in test and quick modes.
cTestsBuild=(de07q)

fCTest_Start(){ fTest_Start "$1" "${cTestName[$1]:?no C test $1}"; }
fCTest_Skip(){
	local why="$1" id
	shift
	for id in "$@"; do fTest_Skip "${id}" "${cTestName[${id}]:?no C test ${id}}" "${why}"; done
}
fCTest_List(){ local id; for id in "$@"; do fEcho_Clean "${id} ${cTestName[${id}]:?no C test ${id}}"; done; }


##	History:
##		- 2026-10-25 JC: Created.
##		- 2026-10-25 JC: The names check, and fCTest_List for --list.
