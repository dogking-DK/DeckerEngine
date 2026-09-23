#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$BuildDir = 'out/build/windows-profiling',
    [string]$Configuration = 'RelWithDebInfo',
    [string]$ToolsDir = 'out/profiling-tools/vcpkg_installed/x64-windows/tools/tracy',
    [ValidateSet('cpu', 'memory', 'memory-disabled')][string]$Mode = 'cpu',
    [string]$MemoryInspector = 'out/profiling-tools/inspector/bin/Release/dk-memory-trace-inspect.exe',
    [ValidateRange(1024, 65535)][int]$Port = 18086
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
function Resolve-RepoPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}
$probeName = if ($Mode -eq 'cpu') { 'dk_profiling_probe' } else { 'dk_memory_probe' }
$probe = Join-Path (Resolve-RepoPath $BuildDir) "bin/$Configuration/$probeName.exe"
$capture = Join-Path (Resolve-RepoPath $ToolsDir) 'tracy-capture.exe'
$exporter = Join-Path (Resolve-RepoPath $ToolsDir) 'tracy-csvexport.exe'
$required = @($probe, $capture, $exporter)
if ($Mode -ne 'cpu') { $required += (Resolve-RepoPath $MemoryInspector) }
foreach ($file in $required) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Required executable missing: $file" }
}
# Fail rather than intentionally connect to an unrelated process using this port.
$portCheck = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Any, $Port)
try { $portCheck.Start() } finally { $portCheck.Stop() }

$runId = (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$runDir = Join-Path $repoRoot "out/profiling/$runId"
New-Item -ItemType Directory -Path $runDir | Out-Null
$traceName = if ($Mode -eq 'cpu') { 'cpu.tracy' } else { 'memory.tracy' }
$tracePath = Join-Path $runDir $traceName
$processes = [Collections.Generic.List[object]]::new()
$summary = [ordered]@{
    status = 'failed'; configuration = $Configuration; port = $Port
    trace = $tracePath; cpu_zones = 0; thread_count = 0; error = $null
    started_at = [DateTimeOffset]::Now.ToString('o'); finished_at = $null
    commit = $null; working_tree = $null; tools_version = $null
    probe = $probe; workload = '128 CPU steps, text boundaries, nested worker and exception unwinding'
    mode = 'on-demand; workload starts after connection; no memory events'
    capture_kind = $Mode; memory_inspection = $null
}
if ($Mode -ne 'cpu') {
    $summary.workload = '36 heap allocations; 2 categories; 2 systems; foreign frees, zero bytes and denied budget'
    $summary.mode = 'on-demand; all allocations and frees inside one connection'
    $summary.cpu_zones = $null; $summary.thread_count = $null
}

function Start-Tool([string]$Name, [string]$Executable, [string[]]$Arguments) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $Executable
    $info.WorkingDirectory = $repoRoot
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    $info.Environment['TRACY_PORT'] = "$Port"
    $info.Environment['TRACY_ONLY_LOCALHOST'] = '1'
    $info.Environment['TRACY_ONLY_IPV4'] = '1'
    $info.Environment['TRACY_NO_EXIT'] = '0'
    $process = [Diagnostics.Process]::Start($info)
    $entry = [pscustomobject]@{
        Name = $Name; Process = $process
        Out = $process.StandardOutput.ReadToEndAsync()
        Err = $process.StandardError.ReadToEndAsync()
    }
    $processes.Add($entry)
    return $entry
}
function Finish-Tool($Entry) {
    if (-not $Entry.Process.WaitForExit(20000)) { throw "$($Entry.Name) timed out after 20 seconds" }
    $stdout = $Entry.Out.GetAwaiter().GetResult()
    $stderr = $Entry.Err.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $runDir "$($Entry.Name).stdout.log"), $stdout)
    [IO.File]::WriteAllText((Join-Path $runDir "$($Entry.Name).stderr.log"), $stderr)
    if ($Entry.Process.ExitCode -ne 0) {
        throw "$($Entry.Name) exited with $($Entry.Process.ExitCode): $stderr $stdout"
    }
    return $stdout
}

try {
    $git = Get-Command git -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($git) {
        $safeRepo = $repoRoot.Replace('\', '/')
        $head = & $git.Source -c "safe.directory=$safeRepo" -C $repoRoot rev-parse HEAD
        if ($LASTEXITCODE -eq 0) { $summary.commit = [string]$head }
        $changes = & $git.Source -c "safe.directory=$safeRepo" -C $repoRoot status --short
        if ($LASTEXITCODE -eq 0) { $summary.working_tree = @($changes) }
    }
    $version = Finish-Tool (Start-Tool 'version' $exporter @('--version'))
    $summary.tools_version = $version.Trim()
    if ($version -notmatch 'tracy-csvexport 0\.14\.1') { throw "Expected matching Tracy 0.14.1 tools: $version" }
    $probeProcess = Start-Tool 'probe' $probe @('--capture')
    # Windows may round each probe sleep to a scheduler tick (~2 s total).
    # Leave time for the capture tool to resolve the final source/name queries.
    $captureProcess = Start-Tool 'capture' $capture @('-a', '127.0.0.1', '-p', "$Port", '-o', $tracePath, '-s', '5')
    $null = Finish-Tool $captureProcess
    $probeOutput = Finish-Tool $probeProcess
    if ($Mode -ne 'cpu') {
        $expected = if ($Mode -eq 'memory') { 'on' } else { 'off' }
        if ($probeOutput.Trim() -ne "memory=$expected;allocations=36") { throw "Unexpected probe output: $probeOutput" }
        $json = Finish-Tool (Start-Tool 'memory-inspect' (Resolve-RepoPath $MemoryInspector) @($tracePath, $expected))
        $inspection = $json | ConvertFrom-Json
        if ($inspection.status -ne 'passed') { throw 'Memory capture inspection failed' }
        $summary.memory_inspection = $inspection
        $summary.status = 'passed'
        Write-Host "Memory capture verified: $($inspection.allocations) allocations; expected=$expected. $tracePath"
        return
    }
    if ($probeOutput -notmatch '^profiling=on;checksum=8128\s*$') { throw "Unexpected probe output: $probeOutput" }
    if ((Get-Item -LiteralPath $tracePath).Length -eq 0) { throw 'Empty Tracy capture' }
    $csv = Finish-Tool (Start-Tool 'export' $exporter @('-u', $tracePath))
    [IO.File]::WriteAllText((Join-Path $runDir 'cpu-zones.csv'), $csv)
    $zones = @($csv | ConvertFrom-Csv)
    foreach ($name in @('Probe.Capture', 'Probe.Worker', 'Probe.Step', 'Probe.Exception')) {
        if (-not ($zones | Where-Object name -EQ $name)) { throw "Missing captured zone: $name" }
    }
    $steps = @($zones | Where-Object name -EQ 'Probe.Step')
    if ($steps.Count -ne 128) { throw "Expected 128 work zones; got $($steps.Count)" }
    foreach ($zone in $zones) {
        if ($zone.src_file -notmatch 'ProfilingProbe\.cpp$' -or [int]$zone.src_line -le 0) {
            throw 'Captured location does not point to the instrumentation call site'
        }
        if ([long]$zone.exec_time_ns -lt 0) { throw 'Unclosed CPU zone in capture' }
    }
    if (-not ($zones | Where-Object { $_.name -eq 'Probe.Capture' -and $_.value -eq 'controlled capture' })) {
        throw 'Missing dynamic zone text'
    }
    foreach ($item in @(
        @{ Name = 'Probe.TemporaryText'; Text = ('t' * 80) },
        @{ Name = 'Probe.EmptyText'; Text = '' },
        @{ Name = 'Probe.LongText'; Text = ('x' * 65534) }
    )) {
        $matching = @($zones | Where-Object { $_.name -eq $item.Name -and $_.value -eq $item.Text })
        if ($matching.Count -ne 1) { throw "Incorrect captured text: $($item.Name)" }
    }
    $threads = @($zones.thread | Sort-Object -Unique)
    if ($threads.Count -ne 2) { throw "Expected main/worker CPU tracks; got $($threads.Count)" }
    $summary.status = 'passed'
    $summary.cpu_zones = $zones.Count
    $summary.thread_count = $threads.Count
    Write-Host "Capture verified: $($zones.Count) zones on $($threads.Count) threads. $tracePath"
} catch {
    $summary.error = $_.Exception.Message
    throw
} finally {
    foreach ($entry in $processes) {
        if (-not $entry.Process.HasExited) {
            $entry.Process.Kill($true)
            $null = $entry.Process.WaitForExit(5000)
        }
        if ($entry.Out.IsCompletedSuccessfully) {
            [IO.File]::WriteAllText((Join-Path $runDir "$($entry.Name).stdout.log"), $entry.Out.Result)
        }
        if ($entry.Err.IsCompletedSuccessfully) {
            [IO.File]::WriteAllText((Join-Path $runDir "$($entry.Name).stderr.log"), $entry.Err.Result)
        }
        $entry.Process.Dispose()
    }
    $summary.finished_at = [DateTimeOffset]::Now.ToString('o')
    [IO.File]::WriteAllText((Join-Path $runDir 'summary.json'), ($summary | ConvertTo-Json -Depth 8) + "`n")
}
