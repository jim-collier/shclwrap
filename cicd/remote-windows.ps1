# Runs shclwrap's Go, Rust, Python and C tests and the shared cases on a
# Windows box, from the tree remote-test.bash sends. One PASS, FAIL or SKIP line
# per test, by its ID. Exits non-zero when any test failed.
# Usage: pwsh -File remote-windows.ps1 -Work <folder holding tree.tgz> -Shcl3 <rev> -Shcl2 <rev> -Py3 <rev> [-KeepTemp 1]
param([string]$Work, [string]$Shcl3, [string]$Shcl2, [string]$Py3, [string]$KeepTemp = '0')
$ErrorActionPreference = 'Continue'
$PSStyle.OutputRendering = 'PlainText'

# Machine use: below normal, half the CPUs, a quarter when busy. Windows has no
# load average, so busy is the CPUs over half used.
(Get-Process -Id $PID).PriorityClass = 'BelowNormal'
$busy = $false
try { $busy = (Get-CimInstance Win32_Processor | Measure-Object -Property LoadPercentage -Average).Average -gt 50 } catch { $busy = $false }
$jobs = [Math]::Max(1, [Math]::Floor([Environment]::ProcessorCount / $(if ($busy) { 4 } else { 2 })))
$env:CARGO_BUILD_JOBS = $jobs; $env:RUST_TEST_THREADS = $jobs; $env:GOMAXPROCS = $jobs
"Jobs: $jobs of $([Environment]::ProcessorCount) CPUs$(if ($busy) { ' (busy)' }), priority $((Get-Process -Id $PID).PriorityClass)"

# An ssh login can get the machine PATH only.
$env:PATH = 'C:\Program Files\Git\cmd;' + (Join-Path $env:USERPROFILE '.cargo\bin') + ';C:\ProgramData\mingw64\mingw64\bin;' + $env:PATH
$py = 'C:\Python314\python.exe'
# Read before TEMP points into the work folder.
$ownRoot = Join-Path $(if ($env:HOST_TEMP) { $env:HOST_TEMP } else { $env:TEMP }) 'shclwrap'
$repo = Join-Path $Work 'repo'
$env:TMP = Join-Path $Work 'tmp'; $env:TEMP = $env:TMP; $env:TMPDIR = $env:TMP
$null = New-Item -ItemType Directory -Force $repo, $env:TMP
$env:PYTHONPYCACHEPREFIX = Join-Path $Work 'pycache'
# Rust builds cold in the work folder. A target folder kept between runs keeps
# test binaries whose CARGO_MANIFEST_DIR is the last run's folder.
$failed = 0; $stepRc = 0

# Runs a native command with its output to <name>.txt, for a converter to
# read, and shows the end of it when it fails. Its exit code is in $stepRc.
function Step($name, [scriptblock]$b) {
	$out = Join-Path $Work "$name.txt"
	& $b *> $out
	$script:stepRc = $LASTEXITCODE
	if ($script:stepRc) { "--- $name exit $($script:stepRc), last lines:"; Get-Content -LiteralPath $out -Tail 30 }
}
# Feeds a file to a converter, which prints the test lines.
function Lines($file, $script, [string[]]$more = @()) {
	Get-Content -LiteralPath $file | & $py -X utf8 (Join-Path $repo "cicd\$script") @more
	if ($LASTEXITCODE) { $script:failed++ }
}

# The safe removal guard, as safe-rm.bash: only one of this project's run
# folders under the box's temp root, by its shape, and never a link or through one.
function SafeRemove([string]$p) {
	$link = [IO.FileAttributes]::ReparsePoint
	if ($p -notmatch ('^' + [regex]::Escape($ownRoot) + '\\\d{8}-\d{6}_\d{3}$') -or $p.Contains('..')) { "safe-rm: refused, not a run folder: $p"; return }
	foreach ($q in $p, $ownRoot) {
		$i = Get-Item -LiteralPath $q -Force -ErrorAction SilentlyContinue
		if (-not $i -or ($i.Attributes -band $link)) { "safe-rm: refused, not there or a link: $q"; return }
	}
	Remove-Item -LiteralPath $p -Recurse -Force
}

# Run folders a run cut off left behind, after 2 days. The box has no run.pid,
# so age alone says one is stale.
Get-ChildItem -LiteralPath $ownRoot -Directory | Where-Object { $_.Name -match '^\d{8}-\d{6}_\d{3}$' -and $_.FullName -ne $Work -and $_.LastWriteTime -lt (Get-Date).AddDays(-2) } |
	ForEach-Object { SafeRemove $_.FullName }

tar -xzf (Join-Path $Work 'tree.tgz') -C $repo
if ($LASTEXITCODE -or -not (Test-Path (Join-Path $repo 'source\go\go.mod'))) { 'FAIL dejex the tree unpacks on Windows'; exit 1 }
$cases = Join-Path $repo 'tests\cases'

Push-Location (Join-Path $repo 'source\go')
Step 'go' { go test -count=1 -json -p $jobs ./... }
$goRc = $stepRc
Lines -file (Join-Path $Work 'go.txt') -script 'go_test_lines.py' -more @('--cases', $cases)
if ($goRc) { $failed++ }
Pop-Location

Push-Location (Join-Path $repo 'source\rust')
Step 'rust' { cargo test --quiet --locked -j $jobs --all-features --tests -- --format pretty }
$rustRc = $stepRc
Lines -file (Join-Path $Work 'rust.txt') -script 'rust_test_lines.py'
if ($rustRc) { $failed++ }
Pop-Location

# Python's own tests at the box's default code page, then its shared cases.
$env:PYTHONPATH = Join-Path $repo "cicd\cache\shcl-py\$Py3"
Push-Location (Join-Path $repo 'source\python')
& $py (Join-Path $repo 'cicd\py_tests.py') tests
if ($LASTEXITCODE) { $failed++ }
Pop-Location
& $py (Join-Path $repo 'cicd\c_cases.py') --lang Python --version 3 --cases $cases --drive $py (Join-Path $repo 'source\python\shclw_drive.py')
if ($LASTEXITCODE) { $failed++ }
Remove-Item Env:PYTHONPATH

# C with the Makefile's warnings, by hand, since the box has no make.
$c = Join-Path $repo 'source\c'; $o = Join-Path $Work 'cbuild'; $null = New-Item -ItemType Directory -Force $o
$i3 = Join-Path $repo "cicd\cache\shcl-c\$Shcl3"; $i2 = Join-Path $repo "cicd\cache\shcl-c\$Shcl2"
$warn = '-Wall', '-Wextra', '-Wpedantic', '-Wshadow', '-Wvla', '-Wconversion', '-Werror'
Push-Location $c
Step 'c-build' {
	$rcs = 0
	foreach ($n in 'shclw', 'info', 'shclw3', 'run', 'shclw-drive') {
		gcc -std=c11 -O1 -g @warn -I. "-I$i3" -c "$n.c" -o (Join-Path $o "$n.o"); if ($LASTEXITCODE) { $rcs = $LASTEXITCODE }
	}
	gcc -std=c11 -O1 -g @warn -I. "-I$i3" -c tests\api_test.c -o (Join-Path $o 'api_test.o'); if ($LASTEXITCODE) { $rcs = $LASTEXITCODE }
	gcc -std=c11 -O1 -g @warn -I. "-I$i2" -c shclw3_v2.c -o (Join-Path $o 'shclw3_v2.o'); if ($LASTEXITCODE) { $rcs = $LASTEXITCODE }
	gcc -std=c11 -O1 -g "-I$i3" -c tests\shcl_impl.c -o (Join-Path $o 'shcl_impl.o'); if ($LASTEXITCODE) { $rcs = $LASTEXITCODE }
	gcc -std=c11 -O1 -g "-I$i2" -c shclw3_v2_impl.c -o (Join-Path $o 'shclw3_v2_impl.o'); if ($LASTEXITCODE) { $rcs = $LASTEXITCODE }
	$lib = 'shclw', 'info', 'shclw3', 'run', 'shclw3_v2', 'shcl_impl', 'shclw3_v2_impl' | ForEach-Object { Join-Path $o "$_.o" }
	gcc @lib (Join-Path $o 'shclw-drive.o') -lm -o (Join-Path $o 'shclw-drive.exe'); if ($LASTEXITCODE) { $rcs = $LASTEXITCODE }
	gcc @lib (Join-Path $o 'api_test.o') -lm -o (Join-Path $o 'api-test.exe'); if ($LASTEXITCODE) { $rcs = $LASTEXITCODE }
	$global:LASTEXITCODE = $rcs
}
$cRc = $stepRc
Pop-Location
if ($cRc) {
	'FAIL dejex C builds on Windows'; $failed++
} else {
	& (Join-Path $o 'api-test.exe')
	if ($LASTEXITCODE) { $failed++ }
	& $py (Join-Path $repo 'cicd\c_cases.py') --drive (Join-Path $o 'shclw-drive.exe') --version 3 --cases $cases
	if ($LASTEXITCODE) { $failed++ }
}

Set-Location $ownRoot
if ($KeepTemp -eq '1') { "kept $Work" } else { SafeRemove $Work }
exit $failed
