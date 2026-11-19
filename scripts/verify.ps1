#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$BuildDir = 'out/build/windows-dev',
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Debug',
    [string[]]$Target = @(),
    [string]$TestRegex = '',
    [switch]$Full,
    [string]$Reason = '',
    [ValidateRange(1, 64)][int]$Parallel = 4
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if ($Full) {
    if ($Target.Count -or $TestRegex -or [string]::IsNullOrWhiteSpace($Reason)) {
        throw 'Use -Full with a nonempty -Reason and without -Target/-TestRegex.'
    }
} elseif ($Target.Count -eq 0 -or [string]::IsNullOrWhiteSpace($TestRegex)) {
    throw 'Targeted verification requires -Target and -TestRegex. No full run is selected by default.'
}
foreach ($name in $Target) {
    if ([string]::IsNullOrWhiteSpace($name) -or $name.StartsWith('-')) {
        throw 'Each target must be a nonempty CMake target name, not an option.'
    }
}
if (-not [System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir = Join-Path $repoRoot $BuildDir }
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)
if (-not (Test-Path -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt'))) {
    throw "Build directory is not configured: $BuildDir. Configure the intended preset first."
}
$cmake = (Get-Command cmake -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
$ctest = (Get-Command ctest -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
$runId = (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$logDir = Join-Path $repoRoot "out/verify/$runId"
New-Item -ItemType Directory -Path $logDir -Force | Out-Null
$summary = [ordered]@{
    started_at = [DateTimeOffset]::Now.ToString('o')
    finished_at = $null
    build_dir = $BuildDir
    configuration = $Configuration
    targets = @($Target)
    full = [bool]$Full
    reason = $Reason
    test_regex = $TestRegex
    commit = $null
    working_tree = $null
    stage = 'metadata'
    build_seconds = $null
    status = 'failed'
    selected_tests = @()
    passed = $null
    failed = $null
    skipped = $null
    error = $null
}

function Invoke-Logged([string]$Command, [string[]]$Arguments, [string]$Log, [string]$ErrorLog = '') {
    # Native stderr may be a PowerShell error record even on a successful command.
    $ErrorActionPreference = 'Continue'
    $PSNativeCommandUseErrorActionPreference = $false
    if ($ErrorLog) { & $Command @Arguments 1> $Log 2> $ErrorLog }
    else { & $Command @Arguments *> $Log }
    $nativeExit = $LASTEXITCODE
    if ($nativeExit -ne 0) {
        Get-Content -LiteralPath $Log -Tail 30 | Out-Host
        if ($ErrorLog) { Get-Content -LiteralPath $ErrorLog -Tail 30 | Out-Host }
    }
    return $nativeExit
}

function Get-GroupedVsBuild {
    if ($Target.Count -lt 2 -or @($Target -notmatch '^dk_[a-zA-Z0-9_]+$').Count) { return }
    $toolInfo = Join-Path $BuildDir 'CMakeFiles/dk-verify-msbuild.txt'
    $solution = Join-Path $BuildDir 'DeckerEngine.slnx'
    $stamps = Join-Path $BuildDir 'CMakeFiles/generate.stamp.list'
    if (-not (Test-Path -LiteralPath $toolInfo) -or -not (Test-Path -LiteralPath $solution) -or
        -not (Test-Path -LiteralPath $stamps) -or (Test-Path -LiteralPath (Join-Path $BuildDir 'CMakeFiles/VerifyGlobs.cmake'))) { return }
    $cache = Get-Content -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt') -Encoding UTF8
    if ($cache -notcontains 'CMAKE_GENERATOR:INTERNAL=Visual Studio 18 2026') { return }

    # Use the same regeneration check as ZERO_CHECK before reading solution folders.
    $sourceDir = ($cache -like 'CMAKE_HOME_DIRECTORY:INTERNAL=*') -replace '^CMAKE_HOME_DIRECTORY:INTERNAL='
    $check = @('-S', $sourceDir, '-B', $BuildDir, '--check-stamp-list', $stamps, '--vs-solution-file', $solution)
    if ((Invoke-Logged $cmake $check (Join-Path $logDir 'regenerate.log')) -ne 0) {
        throw 'CMake regeneration failed. See regenerate.log.'
    }
    $msbuild, $platform = Get-Content -LiteralPath $toolInfo -Encoding UTF8
    if (-not $platform -or -not $msbuild -or -not (Test-Path -LiteralPath $msbuild -PathType Leaf)) { return }

    [xml]$xml = Get-Content -LiteralPath $solution -Raw -Encoding UTF8
    $paths = @{}
    foreach ($project in $xml.SelectNodes('/Solution/Folder/Project | /Solution/Project')) {
        $path = $project.GetAttribute('Path')
        if ([IO.Path]::GetExtension($path) -ne '.vcxproj') { continue }
        $name = [IO.Path]::GetFileNameWithoutExtension($path)
        $folder = $project.ParentNode.GetAttribute('Name').Trim('/').Replace('/', '\')
        $paths[$name] = if ($folder) { "$folder\$name" } else { $name }
    }
    $requested = [System.Collections.Generic.List[string]]::new()
    foreach ($name in $Target) {
        if (-not $paths.ContainsKey($name) -or $paths[$name] -notmatch '^[a-zA-Z0-9_ \\-]+$') { return }
        $requested.Add($paths[$name])
    }
    [pscustomobject]@{
        Command = $msbuild
        Arguments = @($solution, "/p:Configuration=$Configuration", "/p:Platform=$platform",
                      "/m:$Parallel", '/v:minimal', "/t:$($requested -join ';')")
    }
}

$exitCode = 1
try {
    $git = Get-Command git -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($git) {
        $safeRepo = $repoRoot.Replace('\', '/')
        $head = & $git.Source -c "safe.directory=$safeRepo" -C $repoRoot rev-parse HEAD
        if ($LASTEXITCODE -eq 0) { $summary.commit = [string]$head }
        $changes = & $git.Source -c "safe.directory=$safeRepo" -C $repoRoot status --short
        if ($LASTEXITCODE -eq 0) { $summary.working_tree = @($changes) }
    }
    $summary.stage = 'build'
    Write-Host "Building requested scope ($Configuration). Logs: $logDir"
    $timer = [Diagnostics.Stopwatch]::StartNew()
    try {
        $build = Get-GroupedVsBuild
        if ($build) {
            $buildExit = Invoke-Logged $build.Command $build.Arguments (Join-Path $logDir 'build.log')
        } else {
            $buildArgs = @('--build', $BuildDir, '--config', $Configuration, '--parallel', "$Parallel")
            if (-not $Full) { $buildArgs += @('--target') + $Target }
            $buildExit = Invoke-Logged $cmake $buildArgs (Join-Path $logDir 'build.log')
        }
        if ($buildExit -ne 0) { throw 'Build failed; tests were not run.' }
    } finally {
        $summary.build_seconds = [Math]::Round($timer.Elapsed.TotalSeconds, 3)
    }

    $summary.stage = 'discovery'
    $testArgs = @('--test-dir', $BuildDir, '-C', $Configuration)
    if (-not $Full) { $testArgs += @('-R', $TestRegex) }
    $discoveryLog = Join-Path $logDir 'discovery.json'
    # Test discovery can emit DLL/CTest diagnostics on stderr; keep stdout valid JSON.
    $discoveryErrorLog = Join-Path $logDir 'discovery-stderr.log'
    if ((Invoke-Logged $ctest ($testArgs + @('--show-only=json-v1')) $discoveryLog $discoveryErrorLog) -ne 0) {
        throw 'CTest discovery failed; tests were not run.'
    }
    $discovery = Get-Content -LiteralPath $discoveryLog -Raw | ConvertFrom-Json
    $summary.selected_tests = @($discovery.tests | ForEach-Object { $_.name })
    if ($summary.selected_tests.Count -eq 0) { throw 'No tests matched. Refine the filter or enable the intended test target.' }
    Write-Host "Selected $($summary.selected_tests.Count) test(s)."
    $summary.selected_tests | ForEach-Object { Write-Host "  $_" }

    $summary.stage = 'tests'
    $junit = Join-Path $logDir 'results.xml'
    $testLog = Join-Path $logDir 'tests.log'
    $testExit = Invoke-Logged $ctest ($testArgs + @('--output-on-failure', '--no-tests=error', '--output-junit', $junit)) $testLog
    if (-not (Test-Path -LiteralPath $junit)) { throw 'CTest did not produce its JUnit report.' }
    [xml]$report = Get-Content -LiteralPath $junit -Raw -Encoding UTF8
    $cases = @($report.SelectNodes('//testcase'))
    if ($cases.Count -ne $summary.selected_tests.Count) { throw 'CTest result count differs from the selected test count.' }
    $summary.passed = 0
    $summary.failed = 0
    $summary.skipped = 0
    foreach ($case in $cases) {
        if ($case.SelectSingleNode('failure') -or $case.SelectSingleNode('error')) { $summary.failed++ }
        elseif ($case.SelectSingleNode('skipped')) { $summary.skipped++ }
        elseif ($case.GetAttribute('status') -eq 'notrun') { $summary.failed++ }
        else { $summary.passed++ }
    }
    if ($testExit -ne 0 -or $summary.failed -gt 0) { throw 'Selected tests failed. See tests.log and results.xml.' }
    $summary.stage = 'complete'
    $summary.status = 'passed'
    if ($summary.skipped -gt 0) { $summary.status = 'passed_with_skips' }
    if ($summary.passed -eq 0 -and $summary.skipped -gt 0) { $summary.status = 'skipped' }
    $exitCode = 0
} catch {
    $summary.error = $_.Exception.Message
    Write-Host "Verification failed: $($summary.error)"
} finally {
    $summary.finished_at = [DateTimeOffset]::Now.ToString('o')
    $summaryPath = Join-Path $logDir 'summary.json'
    [System.IO.File]::WriteAllText($summaryPath, ($summary | ConvertTo-Json -Depth 8) + "`n", [System.Text.UTF8Encoding]::new($false))
    Write-Host "Status: $($summary.status); passed=$($summary.passed), failed=$($summary.failed), skipped=$($summary.skipped)"
    Write-Host "Summary: $summaryPath"
}
exit $exitCode
