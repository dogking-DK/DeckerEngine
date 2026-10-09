#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$BuildDir = 'out/build/windows-graphics-profiling',
    [string]$Configuration = 'RelWithDebInfo',
    [string]$ToolsDir = 'out/profiling-tools/vcpkg_installed/x64-windows/tools/tracy',
    [ValidateRange(1024,65535)][int]$Port = 18087
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
function Resolve-Repo([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}
$probe = Join-Path (Resolve-Repo $BuildDir) "bin/$Configuration/dk-gpu-profiling-probe.exe"
$capture = Join-Path (Resolve-Repo $ToolsDir) 'tracy-capture.exe'
$exporter = Join-Path (Resolve-Repo $ToolsDir) 'tracy-csvexport.exe'
foreach ($path in @($probe,$capture,$exporter)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing executable: $path" }
}
$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Any,$Port)
try { $listener.Start() } finally { $listener.Stop() }
$runDir = Join-Path $repoRoot ('out/profiling/gpu-' + (Get-Date -Format yyyyMMdd-HHmmss) + '-' + [guid]::NewGuid().ToString('N').Substring(0,8))
$null = New-Item -ItemType Directory -Path $runDir
$trace = Join-Path $runDir 'gpu.tracy'
$measurement = Join-Path $runDir 'gpu-profile.json'
$processes = [Collections.Generic.List[object]]::new()
$summary = [ordered]@{
    status='failed'; configuration=$Configuration; port=$Port; trace=$trace
    started_at=[DateTimeOffset]::Now.ToString('o'); finished_at=$null
    commit=(& git -C $repoRoot rev-parse HEAD); working_tree=@(& git -C $repoRoot status --short)
    gpu_zones=0; cpu_zones=0; error=$null; tools_version=$null
    workload='seed 42, 8x8 cloth, 4 batches x 2 steps, 96x96 draw; OFF/ON byte equality; lifecycle failures'
}
function Start-Tool([string]$Name,[string]$Executable,[string[]]$Arguments) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName=$Executable; $info.WorkingDirectory=$repoRoot
    $info.UseShellExecute=$false; $info.CreateNoWindow=$true; $info.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
    $info.RedirectStandardOutput=$true; $info.RedirectStandardError=$true
    foreach ($arg in $Arguments) { $info.ArgumentList.Add($arg) }
    $info.Environment['TRACY_PORT']="$Port"; $info.Environment['TRACY_ONLY_LOCALHOST']='1'
    $info.Environment['TRACY_ONLY_IPV4']='1'; $info.Environment['TRACY_NO_EXIT']='0'
    $info.Environment['VK_LAYER_VALIDATE_SYNC']='1'; $info.Environment['VK_VALIDATION_VALIDATE_SYNC']='1'
    $process=[Diagnostics.Process]::Start($info)
    $entry=[pscustomobject]@{Name=$Name;Process=$process;Out=$process.StandardOutput.ReadToEndAsync();Err=$process.StandardError.ReadToEndAsync()}
    $processes.Add($entry)
    return $entry
}
function Finish-Tool($Entry) {
    if (-not $Entry.Process.WaitForExit(45000)) { throw "$($Entry.Name) timed out" }
    $stdout=$Entry.Out.GetAwaiter().GetResult(); $stderr=$Entry.Err.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $runDir "$($Entry.Name).stdout.log"),$stdout)
    [IO.File]::WriteAllText((Join-Path $runDir "$($Entry.Name).stderr.log"),$stderr)
    if ($Entry.Process.ExitCode -ne 0) { throw "$($Entry.Name) exited $($Entry.Process.ExitCode): $stderr $stdout" }
    return $stdout
}
try {
    $summary.tools_version=(Finish-Tool (Start-Tool 'version' $exporter @('--version'))).Trim()
    if ($summary.tools_version -notmatch 'tracy-csvexport 0\.14\.1') { throw 'Expected Tracy 0.14.1 tools' }
    $probeRun=Start-Tool 'probe' $probe @('--capture',$measurement)
    $captureRun=Start-Tool 'capture' $capture @('-a','127.0.0.1','-p',"$Port",'-o',$trace,'-s','20')
    $null=Finish-Tool $captureRun
    $null=Finish-Tool $probeRun
    $measured=Get-Content -Raw -LiteralPath $measurement | ConvertFrom-Json
    if ($measured.status -ne 'passed' -or -not $measured.tracy_enabled -or $measured.steps -ne 8) { throw 'Invalid probe report' }
    $gpuCsv=Finish-Tool (Start-Tool 'gpu-export' $exporter @('-g',$trace))
    $cpuCsv=Finish-Tool (Start-Tool 'cpu-export' $exporter @('-u',$trace))
    [IO.File]::WriteAllText((Join-Path $runDir 'gpu-zones.csv'),$gpuCsv)
    [IO.File]::WriteAllText((Join-Path $runDir 'cpu-zones.csv'),$cpuCsv)
    $gpu=@($gpuCsv | ConvertFrom-Csv); $cpu=@($cpuCsv | ConvertFrom-Csv)
    foreach ($name in @('GPU submission','XPBD predict #2','cloth visualization')) {
        if (-not @($gpu | Where-Object name -EQ $name).Count) { throw "Missing GPU zone: $name" }
    }
    foreach ($name in @('Shader.compile','physics.gpu.graph_build','graph.compile','graph.execute','graphics.submit','graphics.wait','graphics.collect')) {
        if (-not @($cpu | Where-Object name -EQ $name).Count) { throw "Missing CPU zone: $name" }
    }
    foreach ($zone in $gpu) {
        if ([long]$zone.'GPU execution time' -lt 0) { throw 'Unclosed GPU interval' }
        if ($zone.name -in @('abandoned','failed submit','failed graph','omitted','nested')) { throw "Unexpected failed/omitted zone: $($zone.name)" }
    }
    # Compare each recorded workload pass against native timestamp measurements;
    # Tracy rounds timestamp endpoints to integer ns, allowing two ns difference.
    $native=@($measured.profiles | ForEach-Object zones | Where-Object kind -NE 'submission')
    foreach ($group in ($native | Group-Object name)) {
        $expected=@($group.Group | Sort-Object gpu_ns)
        $actual=@($gpu | Where-Object name -EQ $group.Name | Sort-Object { [long]$_.'GPU execution time' })
        if ($expected.Count -ne $actual.Count) { throw "GPU zone count differs: $($group.Name)" }
        for ($i=0;$i -lt $expected.Count;++$i) {
            if ([math]::Abs([double]$expected[$i].gpu_ns - [long]$actual[$i].'GPU execution time') -gt 2) { throw "GPU duration differs: $($group.Name)" }
        }
    }
    foreach ($profile in $measured.profiles) {
        foreach ($name in @('graph.execute','graphics.submit','graphics.wait')) {
            if (-not @($cpu | Where-Object { $_.name -eq $name -and [long](($_.value -split ' ',2)[0]) -eq $profile.submission }).Count) {
                throw "Missing submission correlation: $name / $($profile.submission)"
            }
        }
    }
    $summary.gpu_zones=$gpu.Count; $summary.cpu_zones=$cpu.Count; $summary.status='passed'
    Write-Host "GPU capture verified: $($gpu.Count) GPU zones; $($cpu.Count) CPU zones. $trace"
} catch {
    $summary.error=$_.Exception.Message
    throw
} finally {
    foreach ($entry in $processes) {
        if (-not $entry.Process.HasExited) { $entry.Process.Kill($true); $entry.Process.WaitForExit() }
        [IO.File]::WriteAllText((Join-Path $runDir "$($entry.Name).stdout.log"),$entry.Out.GetAwaiter().GetResult())
        [IO.File]::WriteAllText((Join-Path $runDir "$($entry.Name).stderr.log"),$entry.Err.GetAwaiter().GetResult())
        $entry.Process.Dispose()
    }
    $summary.finished_at=[DateTimeOffset]::Now.ToString('o')
    $summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $runDir 'summary.json') -Encoding utf8
}
