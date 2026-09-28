#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$OffBuildDir = 'out/build/windows-dev',
    [string]$CpuBuildDir = 'out/build/windows-profiling-cpu-only',
    [string]$MemoryBuildDir = 'out/build/windows-profiling',
    [ValidateSet('RelWithDebInfo', 'Release')][string]$Configuration = 'RelWithDebInfo',
    [ValidateRange(1, 100000)][int]$Rounds = 512,
    [ValidateRange(1, 10000)][int]$Warmup = 32,
    [ValidateRange(1, 256)][int]$Batch = 16,
    [ValidateRange(1, 128)][int]$MaxThreads = [Math]::Min(128, [Environment]::ProcessorCount),
    [ValidateRange(1, 10)][int]$Repetitions = 3,
    [ValidateRange(1024, 65000)][int]$Port = 18100,
    [ValidateRange(10, 1800)][int]$TimeoutSeconds = 300,
    [string]$ToolsDir = 'out/profiling-tools/vcpkg_installed/x64-windows/tools/tracy',
    [string]$Inspector = 'out/profiling-tools/inspector/bin/Release/dk-memory-trace-inspect.exe'
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
function Resolve-RepoPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}
function Write-Json([string]$Path, $Value) {
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 12) + "`n", [Text.UTF8Encoding]::new($false))
}
function Read-Cache([string]$Path) {
    $values = @{}
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line -match '^([^#/][^:]*):[^=]*=(.*)$') { $values[$Matches[1]] = $Matches[2] }
    }
    return $values
}
function Median($Values) {
    $sorted = @($Values | Sort-Object)
    if ($sorted.Count % 2) { return [double]$sorted[[int][Math]::Floor($sorted.Count / 2)] }
    return ([double]$sorted[$sorted.Count / 2 - 1] + [double]$sorted[$sorted.Count / 2]) / 2
}
function Case-Key($Value) { return @($Value.strategy, $Value.transfer, $Value.threads, $Value.bytes, $Value.alignment) -join '/' }

$runId = (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$runDir = Join-Path $repoRoot "out/benchmarks/$runId"
New-Item -ItemType Directory -Path $runDir -Force | Out-Null
$capture = Join-Path (Resolve-RepoPath $ToolsDir) 'tracy-capture.exe'
$exporter = Join-Path (Resolve-RepoPath $ToolsDir) 'tracy-csvexport.exe'
$inspectorPath = Resolve-RepoPath $Inspector
$builds = [ordered]@{ off = $OffBuildDir; cpu = $CpuBuildDir; memory = $MemoryBuildDir }
$summary = [ordered]@{
    schema = 1; status = 'failed'; error = $null; started_at = [DateTimeOffset]::Now.ToString('o'); finished_at = $null
    configuration = $Configuration; rounds = $Rounds; warmup = $Warmup; batch = $Batch; max_threads = $MaxThreads
    repetitions = $Repetitions; commit = $null; working_tree = $null; machine = $null; manifest = $null
    tools_version = $null; builds = [ordered]@{}; runs = [Collections.Generic.List[object]]::new()
}
$processes = [Collections.Generic.List[object]]::new()
function Start-Tool([string]$Name, [string]$Executable, [string[]]$Arguments, [string]$Directory, [int]$RunPort) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $Executable; $info.WorkingDirectory = $repoRoot; $info.UseShellExecute = $false
    $info.CreateNoWindow = $true; $info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    $info.Environment['TRACY_PORT'] = "$RunPort"; $info.Environment['TRACY_ONLY_LOCALHOST'] = '1'
    $info.Environment['TRACY_ONLY_IPV4'] = '1'; $info.Environment['TRACY_NO_EXIT'] = '0'
    $process = [Diagnostics.Process]::Start($info)
    $entry = [pscustomobject]@{
        Name = $Name; Process = $process; Directory = $Directory
        Out = $process.StandardOutput.ReadToEndAsync(); Err = $process.StandardError.ReadToEndAsync()
    }
    $processes.Add($entry)
    return $entry
}
function Finish-Tool($Entry) {
    if (-not $Entry.Process.WaitForExit($TimeoutSeconds * 1000)) { throw "$($Entry.Name) timed out" }
    $stdout = $Entry.Out.GetAwaiter().GetResult(); $stderr = $Entry.Err.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $Entry.Directory "$($Entry.Name).stdout.log"), $stdout)
    [IO.File]::WriteAllText((Join-Path $Entry.Directory "$($Entry.Name).stderr.log"), $stderr)
    if ($Entry.Process.ExitCode -ne 0) { throw "$($Entry.Name) exited with $($Entry.Process.ExitCode): $stderr" }
    return $stdout
}

try {
    foreach ($tool in @($capture, $exporter, $inspectorPath)) {
        if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) { throw "Missing tool: $tool" }
    }
    $summary.commit = (& git -C $repoRoot rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Cannot read baseline commit' }
    $summary.working_tree = @(& git -C $repoRoot status --short)
    $summary.manifest = Get-Content -LiteralPath (Join-Path $repoRoot 'vcpkg.json') -Raw | ConvertFrom-Json
    $summary.machine = [ordered]@{
        processors = @(Get-CimInstance Win32_Processor | Select-Object Name, NumberOfCores, NumberOfLogicalProcessors)
        os = Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, BuildNumber, TotalVisibleMemorySize
        process_architecture = [Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture.ToString()
        logical_processors = [Environment]::ProcessorCount
        power_plan = (& powercfg /getactivescheme | Out-String).Trim()
    }
    $version = Finish-Tool (Start-Tool 'tools-version' $exporter @('--version') $runDir $Port)
    if ($version -notmatch 'tracy-csvexport 0\.14\.1') { throw 'Expected Tracy 0.14.1 tools' }
    $summary.tools_version = $version.Trim()
    $reference = $null
    foreach ($mode in $builds.Keys) {
        $build = Resolve-RepoPath $builds[$mode]
        $cache = Read-Cache (Join-Path $build 'CMakeCache.txt')
        $binary = Join-Path $build "bin/$Configuration/dk_memory_benchmark.exe"
        if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) { throw "Build dk_memory_benchmark first: $binary" }
        $profileExpected = if ($mode -eq 'off') { 'OFF' } else { 'ON' }
        $memoryExpected = if ($mode -eq 'cpu') { 'OFF' } else { 'ON' }
        if ($cache.DK_ENABLE_PROFILING -ne $profileExpected -or $cache.DK_PROFILE_MEMORY -ne $memoryExpected -or
            $cache.DK_PROFILE_CALLSTACK_DEPTH -ne '0' -or $cache.DK_BUILD_MEMORY -ne 'ON') { throw "Wrong profiling configuration: $mode" }
        $configKey = 'CMAKE_CXX_FLAGS_' + $Configuration.ToUpperInvariant()
        $settings = [ordered]@{}
        foreach ($key in @('CMAKE_CXX_COMPILER', 'CMAKE_GENERATOR', 'CMAKE_GENERATOR_PLATFORM', 'CMAKE_CXX_FLAGS', $configKey, 'VCPKG_TARGET_TRIPLET')) {
            $settings[$key] = $cache[$key]
        }
        $identity = $settings | ConvertTo-Json -Compress
        if ($reference -and $identity -ne $reference) { throw 'Compiler, architecture or optimization flags differ across modes' }
        $reference = $identity
        $summary.builds[$mode] = [ordered]@{
            directory = $build; binary = $binary; sha256 = (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash
            settings = $settings; profiling = $cache.DK_ENABLE_PROFILING; memory = $cache.DK_PROFILE_MEMORY
            runtime_dlls = @(Get-ChildItem -LiteralPath (Split-Path -Parent $binary) -Filter '*.dll' | ForEach-Object {
                [pscustomobject]@{ name = $_.Name; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
            })
        }
        Copy-Item -LiteralPath (Join-Path $build 'CMakeCache.txt') -Destination (Join-Path $runDir "$mode-CMakeCache.txt")
    }
    $sourceHashes = foreach ($path in @('tests/integration/MemoryBenchmark.cpp', 'scripts/benchmark-memory.ps1', 'tools/profiling/inspector/MemoryTraceInspector.cpp')) {
        [pscustomobject]@{ path = $path; sha256 = (Get-FileHash -LiteralPath (Join-Path $repoRoot $path) -Algorithm SHA256).Hash }
    }
    Write-Json (Join-Path $runDir 'source-hashes.json') @($sourceHashes)
    $allRows = [Collections.Generic.List[object]]::new()
    $expectedChecksums = @{}
    $threadCounts = @(1, 2, 4, $MaxThreads | Where-Object { $_ -le $MaxThreads } | Sort-Object -Unique)
    $expectedCases = 3 * (5 * $threadCounts.Count + 3 * ($threadCounts.Count - 1)) + 1
    $modes = @('off', 'cpu', 'memory')
    for ($repeat = 0; $repeat -lt $Repetitions; ++$repeat) {
        for ($offset = 0; $offset -lt $modes.Count; ++$offset) {
            $mode = $modes[($repeat + $offset) % $modes.Count]
            $runPort = $Port + $repeat * 3 + $offset
            $directory = Join-Path $runDir "$($repeat + 1)-$mode"
            New-Item -ItemType Directory -Path $directory | Out-Null
            Write-Host "Baseline $($repeat + 1)/$Repetitions $mode ($expectedCases cases). Logs: $directory"
            $argsList = @('--rounds', "$Rounds", '--warmup', "$Warmup", '--batch', "$Batch", '--max-threads', "$MaxThreads")
            $trace = Join-Path $directory 'memory.tracy'
            $captureProcess = $null
            if ($mode -ne 'off') {
                $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Any, $runPort)
                try { $listener.Start() } finally { $listener.Stop() }
                $argsList += '--capture'
            }
            $probe = Start-Tool 'benchmark' $summary.builds[$mode].binary $argsList $directory $runPort
            if ($mode -ne 'off') { $captureProcess = Start-Tool 'capture' $capture @('-a', '127.0.0.1', '-p', "$runPort", '-o', $trace) $directory $runPort }
            $output = Finish-Tool $probe
            if ($captureProcess) { $null = Finish-Tool $captureProcess }
            [IO.File]::WriteAllText((Join-Path $directory 'measurements.jsonl'), $output)
            $lines = @($output -split '\r?\n' | Where-Object { $_.Trim() } | ForEach-Object { $_ | ConvertFrom-Json })
            $metadata = @($lines | Where-Object kind -EQ 'metadata'); $total = @($lines | Where-Object kind -EQ 'summary')
            $rows = @($lines | Where-Object kind -EQ 'case')
            if ($metadata.Count -ne 1 -or $total.Count -ne 1 -or $total[0].status -ne 'passed' -or
                $rows.Count -ne $expectedCases -or $total[0].case_count -ne $expectedCases) { throw 'Incomplete benchmark result' }
            $meta = $metadata[0]; $totals = $total[0]
            if ($meta.configuration -ne $Configuration -or $meta.profiling -ne ($mode -ne 'off') -or
                $meta.memory -ne ($mode -eq 'memory') -or $meta.connected -ne ($mode -ne 'off') -or
                $meta.rounds -ne $Rounds -or $meta.warmup -ne $Warmup -or $meta.batch -ne $Batch -or $meta.max_threads -ne $MaxThreads) {
                throw 'Runtime metadata differs from the requested baseline'
            }
            $seen = @{}; [long]$minimumCross = $totals.assets_allocations
            foreach ($row in $rows) {
                $key = Case-Key $row
                if ($seen.ContainsKey($key)) { throw "Duplicate case: $key" }; $seen[$key] = $true
                if ($row.final_live_allocations -ne 0 -or $row.final_backing_bytes -ne 0 -or $row.elapsed_ns -le 0 -or
                    $row.requests_per_second -le 0 -or $row.batch_p50_ns -gt $row.batch_p95_ns -or $row.batch_p95_ns -gt $row.batch_p99_ns) {
                    throw "Invalid lifecycle or timing result: $key"
                }
                $expectedRequests = if ($row.strategy -eq 'pipeline') { $Rounds } else { $row.threads * $Rounds * $Batch }
                if ($row.requests -ne $expectedRequests) { throw "Invalid request count: $key" }
                if ($expectedChecksums.ContainsKey($key) -and $row.checksum -ne $expectedChecksums[$key]) { throw "Cross-mode checksum mismatch: $key" }
                $expectedChecksums[$key] = $row.checksum
                if ($row.transfer -eq 'cross' -and $row.strategy -in @('heap', 'pmr')) { $minimumCross += $row.backing_allocations }
                $row | Add-Member -NotePropertyName mode -NotePropertyValue $mode
                $row | Add-Member -NotePropertyName repetition -NotePropertyValue ($repeat + 1)
                $allRows.Add($row)
            }
            $measurement = [ordered]@{
                status = $totals.status; memory_enabled = ($mode -eq 'memory'); case_count = $rows.Count
                backing_allocations = $totals.backing_allocations; jobs_allocations = $totals.jobs_allocations
                assets_allocations = $totals.assets_allocations; minimum_cross_frees = $minimumCross
            }
            if (($rows | Measure-Object backing_allocations -Sum).Sum -ne $totals.backing_allocations) { throw 'Backing counter sum mismatch' }
            $measurementPath = Join-Path $directory 'inspection-input.json'; Write-Json $measurementPath $measurement
            $inspection = $null; $cpuCount = 0
            if ($mode -ne 'off') {
                $enabled = if ($mode -eq 'memory') { 'on' } else { 'off' }
                $inspection = (Finish-Tool (Start-Tool 'inspect' $inspectorPath @($trace, $enabled, 'benchmark', $measurementPath) $directory $runPort)) | ConvertFrom-Json
                if ($inspection.status -ne 'passed') { throw 'Trace inspection failed' }
                $csv = Finish-Tool (Start-Tool 'export' $exporter @('-u', $trace) $directory $runPort)
                [IO.File]::WriteAllText((Join-Path $directory 'cpu-zones.csv'), $csv)
                $zones = @($csv | ConvertFrom-Csv); $cpuCount = $zones.Count
                $expectedBatches = ($rows | Where-Object strategy -NE 'pipeline' | ForEach-Object { $_.threads * ($Rounds + $Warmup) } | Measure-Object -Sum).Sum
                foreach ($expected in @(
                    @{ name = 'Memory.Benchmark.Case'; count = $rows.Count - 1 },
                    @{ name = 'Memory.Benchmark.Batch'; count = $expectedBatches },
                    @{ name = 'Memory.Benchmark.Pipeline'; count = 1 },
                    @{ name = 'Memory.Benchmark.Import'; count = $Rounds + $Warmup }
                )) {
                    if (@($zones | Where-Object name -EQ $expected.name).Count -ne $expected.count) { throw "Unexpected CPU zone count: $($expected.name)" }
                }
                foreach ($zone in $zones) {
                    if ([long]$zone.exec_time_ns -lt 0 -or [int]$zone.src_line -le 0) { throw 'Unclosed CPU zone or missing source location' }
                }
            }
            $summary.runs.Add([ordered]@{ mode = $mode; repetition = $repeat + 1; directory = $directory; metadata = $meta; cases = $rows.Count; cpu_zones = $cpuCount; inspection = $inspection })
            Write-Json (Join-Path $runDir 'summary.json') $summary
        }
    }
    $aggregate = [Collections.Generic.List[object]]::new()
    foreach ($group in ($allRows | Group-Object { (Case-Key $_) + '/' + $_.mode })) {
        $values = @($group.Group); $first = $values[0]
        if ($values.Count -ne $Repetitions) { throw 'Missing repetitions' }
        $aggregate.Add([pscustomobject][ordered]@{
            strategy = $first.strategy; transfer = $first.transfer; threads = $first.threads; bytes = $first.bytes; alignment = $first.alignment; mode = $first.mode
            repetitions = $values.Count; requests = $first.requests; checksum = $first.checksum
            throughput_median = Median $values.requests_per_second
            throughput_min = ($values.requests_per_second | Measure-Object -Minimum).Minimum
            throughput_max = ($values.requests_per_second | Measure-Object -Maximum).Maximum
            elapsed_ns_median = Median $values.elapsed_ns
            batch_p50_ns_median = Median $values.batch_p50_ns; batch_p95_ns_median = Median $values.batch_p95_ns; batch_p99_ns_median = Median $values.batch_p99_ns
            heap_peak_bytes_max = ($values.heap_peak_bytes | Measure-Object -Maximum).Maximum; heap_peak_kind = $first.heap_peak_kind
            retained_bytes_median = Median $values.retained_bytes; local_logical_peak_sum_max = ($values.local_logical_peak_sum | Measure-Object -Maximum).Maximum
            backing_allocations_median = Median $values.backing_allocations; elapsed_ratio_to_off = 1.0
        })
    }
    $off = @{}; foreach ($row in $aggregate) { if ($row.mode -eq 'off') { $off[(Case-Key $row)] = $row.elapsed_ns_median } }
    foreach ($row in $aggregate) { $row.elapsed_ratio_to_off = $row.elapsed_ns_median / $off[(Case-Key $row)] }
    $sortedRows = @($aggregate | Sort-Object strategy, transfer, threads, bytes, mode)
    $sortedRows | Export-Csv -LiteralPath (Join-Path $runDir 'baseline.csv') -NoTypeInformation -Encoding utf8
    Write-Json (Join-Path $runDir 'baseline.json') $sortedRows
    $summary.status = 'passed'
    $report = [Collections.Generic.List[string]]::new()
    $report.Add('# Memory baseline')
    $report.Add('')
    $report.Add("Run: $runId; commit: $($summary.commit); configuration: $Configuration.")
    $report.Add("CPU: $($summary.machine.processors[0].Name); logical processors: $($summary.machine.logical_processors).")
    $report.Add("Rounds: $Rounds; warmup: $Warmup; batch: $Batch; repetitions: $Repetitions; N: $MaxThreads; cases per process: $expectedCases.")
    $report.Add('')
    $report.Add('Selected 256-byte / 64-byte-aligned cases. Full matrix, run ranges and percentile medians: baseline.csv.')
    $report.Add('Throughput is checked logical requests/second; pipeline uses outputs/second. Latency is per worker batch, not per allocation.')
    $report.Add('Cross-thread cases include two barriers per batch. Arena reclaims the whole batch. Timing excludes setup, warmup and cleanup.')
    $report.Add('Backing counters/peaks include warmup. Pipeline heap peak is the sum of two domain peaks, not a simultaneous total; retained is local backing.')
    $report.Add('CPU and Memory runs have a live capture process. Ratios include profiler synchronization, transport and local collector contention.')
    $report.Add('')
    $report.Add('| Strategy | Transfer | Threads | OFF M req/s | CPU/OFF time | Memory/OFF time | OFF p95 batch us | OFF peak bytes | OFF retained bytes |')
    $report.Add('| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |')
    foreach ($row in $sortedRows | Where-Object { $_.mode -eq 'off' -and $_.bytes -eq 256 -and $_.threads -in @(1, $MaxThreads) }) {
        $key = Case-Key $row
        $cpu = $sortedRows | Where-Object { $_.mode -eq 'cpu' -and (Case-Key $_) -eq $key }
        $memory = $sortedRows | Where-Object { $_.mode -eq 'memory' -and (Case-Key $_) -eq $key }
        $report.Add(('| {0} | {1} | {2} | {3:F3} | {4:F3} | {5:F3} | {6:F3} | {7} | {8} |' -f $row.strategy, $row.transfer, $row.threads,
            ($row.throughput_median / 1e6), $cpu.elapsed_ratio_to_off, $memory.elapsed_ratio_to_off, ($row.batch_p95_ns_median / 1000), $row.heap_peak_bytes_max, $row.retained_bytes_median))
    }
    $report.Add('')
    $report.Add('No speed threshold. Synthetic checked workloads, unpinned threads and uncontrolled system load; compare ranges before drawing conclusions.')
    [IO.File]::WriteAllText((Join-Path $runDir 'report.md'), ($report -join "`n") + "`n", [Text.UTF8Encoding]::new($false))
    Write-Host "Baseline passed: $($summary.runs.Count) runs; $($aggregate.Count) aggregate rows. $runDir"
} catch {
    $summary.status = 'failed'
    $summary.error = $_.Exception.Message
    throw
} finally {
    foreach ($entry in $processes) {
        if (-not $entry.Process.HasExited) { $entry.Process.Kill($true); $null = $entry.Process.WaitForExit(5000) }
        if ($entry.Out.IsCompletedSuccessfully) { [IO.File]::WriteAllText((Join-Path $entry.Directory "$($entry.Name).stdout.log"), $entry.Out.Result) }
        if ($entry.Err.IsCompletedSuccessfully) { [IO.File]::WriteAllText((Join-Path $entry.Directory "$($entry.Name).stderr.log"), $entry.Err.Result) }
        $entry.Process.Dispose()
    }
    $summary.finished_at = [DateTimeOffset]::Now.ToString('o')
    Write-Json (Join-Path $runDir 'summary.json') $summary
}
