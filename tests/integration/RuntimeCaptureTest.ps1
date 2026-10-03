param([Parameter(Mandatory=$true)][string]$Runner,
      [Parameter(Mandatory=$true)][string]$Fixtures,
      [Parameter(Mandatory=$true)][string]$WorkDir)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Join-Path $WorkDir ('capture-' + [guid]::NewGuid().ToString('N').Substring(0,16) + ' 测试')
New-Item -ItemType Directory -Force -Path $root | Out-Null
Copy-Item -Path (Join-Path $Fixtures '*') -Destination $root -Recurse
$utf8 = [System.Text.UTF8Encoding]::new($false)
$script:requestId = 0
function Check([bool]$ok, [string]$message) { if (-not $ok) { throw $message } }
function Start-Server {
    $info = [System.Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $Runner
    $info.Arguments = '--project-root "' + $root + '" --stdio'
    $info.UseShellExecute = $false; $info.CreateNoWindow = $true
    $info.RedirectStandardInput = $true; $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = $utf8; $info.StandardErrorEncoding = $utf8
    $process = [System.Diagnostics.Process]::new(); $process.StartInfo = $info
    [void]$process.Start()
    $writer = [System.IO.StreamWriter]::new($process.StandardInput.BaseStream,$utf8); $writer.AutoFlush = $true
    return [pscustomobject]@{ Process=$process; Writer=$writer; Errors=$process.StandardError.ReadToEndAsync() }
}
function Call-Rpc($server,[string]$method,$parameters=@{}) {
    $script:requestId++
    $server.Writer.WriteLine((@{jsonrpc='2.0';id=$script:requestId;method=$method;params=$parameters} | ConvertTo-Json -Depth 32 -Compress))
    $read = $server.Process.StandardOutput.ReadLineAsync()
    Check ($read.Wait(10000)) ('Response timeout: '+$method)
    Check ($null -ne $read.Result) ('Unexpected EOF: '+$method)
    $response = $read.Result | ConvertFrom-Json
    Check ($response.id -eq $script:requestId) 'Wrong response id'
    return $response
}
function Value($response) {
    Check ($response.PSObject.Properties.Name -contains 'result') ('Expected success: '+($response | ConvertTo-Json -Depth 32 -Compress))
    Check ($response.result.status -eq 'succeeded') 'Wrong TaskId terminal status'
    return $response.result.value
}
function Wait-Job($server,[string]$id) {
    $deadline = [DateTime]::UtcNow.AddSeconds(45)
    do {
        $waited = Value (Call-Rpc $server 'jobs.wait' @{id=$id;timeout_ms=1000})
        if (-not $waited.timed_out) { return $waited.job }
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'Background job did not reach a terminal state'
}
function Scene-Guard($state) { return @{document_id=$state.document_id;revision=$state.revision} }
function Finish($server) {
    Check ($server.Process.WaitForExit(10000)) 'Process did not join on shutdown/EOF'
    Check ($server.Process.ExitCode -eq 0) 'Unexpected exit code'
    Check ($server.Errors.Wait(2000)) 'stderr did not close'
    Check ($server.Errors.Result -eq '') ('Unexpected diagnostics: '+$server.Errors.Result)
    Check ($server.Process.StandardOutput.ReadToEnd() -eq '') 'Unexpected trailing stdout'
}
function Dispose-Server($server) {
    if ($null -eq $server) { return }
    if (-not $server.Process.HasExited) { $server.Process.Kill(); [void]$server.Process.WaitForExit(3000) }
    $server.Writer.Dispose(); $server.Process.Dispose()
}

function Capture($server,$state,[string]$output) {
    return Value (Call-Rpc $server 'render.capture' @{guard=(Scene-Guard $state);output=$output;width=64;height=64;
        camera=@{eye=@(0,0,-2);target=@(0,0,0);fov_y=60};validation='required'})
}
function Digest([string]$name) { return (Get-FileHash -LiteralPath (Join-Path $root $name) -Algorithm SHA256).Hash }
$server = $null
try {
    $server = Start-Server
    $caps = Value (Call-Rpc $server 'runtime.capabilities')
    Check ($caps.render_capture -and $caps.async_jobs -and -not $caps.async_tasks) 'Capture capability missing'
    $commands = @(Value (Call-Rpc $server 'commands.list'))
    Check (@($commands | Where-Object name -eq 'render.capture').Count -eq 1) 'Capture discovery missing'
    foreach ($name in @('render.capture','jobs.get','jobs.wait','jobs.cancel')) {
        $description = Value (Call-Rpc $server 'commands.describe' @{name=$name})
        Check (-not $description.undoable -and -not $description.parameters.additionalProperties) ('Invalid schema: '+$name)
        [System.IO.File]::WriteAllText((Join-Path $root ($name+'.schema.json')),($description | ConvertTo-Json -Depth 64),$utf8)
    }
    $state = Value (Call-Rpc $server 'scene.load' @{manifest='project.json'})
    $diskScene = Digest 'scene.json'
    $first = Capture $server $state 'first.ppm'
    $job = Wait-Job $server $first.job_id
    if ($job.state -eq 'failed' -and $job.error.code -in @(3,5)) {
        # Device unavailability is explicitly skipped only if the diagnostic identifies the device layer.
        $reason = $job.error.message
        if ($reason -match 'Vulkan|adapter|validation layer|loader') { Write-Output ('SKIP: '+$reason); exit 77 }
    }
    Check ($job.state -eq 'succeeded') ('Initial capture failed: '+($job | ConvertTo-Json -Depth 16 -Compress))
    Check ($job.result.kind -eq 'capture' -and $job.result.draw_count -eq 3) 'Capture did not draw fixture'
    Check ($job.result.frame -eq $first.frame -and $job.result.revision -eq $state.revision) 'Initial frame/revision mismatch'
    $image = [System.IO.File]::ReadAllBytes((Join-Path $root 'first.ppm'))
    $header = [System.Text.Encoding]::ASCII.GetBytes("P6`n64 64`n255`n")
    Check ($image.Length -eq ($header.Length+64*64*3)) 'Invalid PPM dimensions or payload'
    Check ([System.Text.Encoding]::ASCII.GetString($image,0,$header.Length) -eq "P6`n64 64`n255`n") 'Invalid PPM header'
    $colors = [System.Collections.Generic.HashSet[string]]::new()
    for ($i=$header.Length;$i -lt $image.Length;$i+=3) { [void]$colors.Add(($image[$i..($i+2)] -join ',')) }
    Check ($colors.Count -ge 3) 'Capture is uniform or missing textured foreground'
    # Capture again, then edit immediately: the accepted snapshot must equal first.ppm.
    $frozen = Capture $server $state 'frozen.ppm'
    $edited = Value (Call-Rpc $server 'entity.set_transform' @{guard=(Scene-Guard $state);id='20000000-0000-4000-8000-000000000001';
        transform=@{translation=@(3,-0.125,0.25);rotation=@(0,0,0,1);scale=@(0.5,0.5,1)}})
    $state2 = $edited.state
    $second = Capture $server $state2 'edited.ppm'
    $frozenJob = Wait-Job $server $frozen.job_id
    $secondJob = Wait-Job $server $second.job_id
    Check ($frozenJob.state -eq 'succeeded' -and $secondJob.state -eq 'succeeded') 'Snapshot captures failed'
    Check ($frozenJob.result.revision -eq $state.revision -and $secondJob.result.revision -eq $state2.revision) 'Capture read a later document version'
    Check ($second.frame -gt $frozen.frame -and $frozen.frame -gt $first.frame) 'Frame sequence did not advance'
    Check ((Digest 'first.ppm') -eq (Digest 'frozen.ppm')) 'Pending capture observed later edits'
    Check ((Digest 'first.ppm') -ne (Digest 'edited.ppm')) 'Unsaved transform did not affect image'
    Check ((Digest 'scene.json') -eq $diskScene) 'Capture saved the Scene implicitly'
    # Idle Runtime must publish without receiving a jobs query.
    $idle = Capture $server $state2 'idle.ppm'
    $deadline = [DateTime]::UtcNow.AddSeconds(45)
    while (-not (Test-Path -LiteralPath (Join-Path $root 'idle.ppm')) -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 20 }
    Check (Test-Path -LiteralPath (Join-Path $root 'idle.ppm')) 'Idle input blocked capture publication'
    Check ((Value (Call-Rpc $server 'jobs.get' @{id=$idle.job_id})).state -eq 'succeeded') 'Published file has no terminal success'
    # An existing output remains intact on importer and atomic replacement failure.
    $saved = Digest 'first.ppm'
    $texture = Join-Path $root 'assets/mask.png'
    $textureBytes = [System.IO.File]::ReadAllBytes($texture)
    Remove-Item -LiteralPath $texture
    $failed = Capture $server $state2 'first.ppm'
    Check ((Wait-Job $server $failed.job_id).state -eq 'failed') 'Import error lacked terminal failure'
    Check ((Digest 'first.ppm') -eq $saved) 'Failed import replaced previous image'
    [System.IO.File]::WriteAllBytes($texture,$textureBytes)
    # Deny replacement but allow reading; the final atomic commit must fail and clean its temporary.
    $locked = [System.IO.File]::Open((Join-Path $root 'first.ppm'),[System.IO.FileMode]::Open,[System.IO.FileAccess]::Read,[System.IO.FileShare]::Read)
    try {
        $blocked = Capture $server $state2 'first.ppm'
        Check ((Wait-Job $server $blocked.job_id).state -eq 'failed') 'Locked output did not fail at publication'
    } finally { $locked.Dispose() }
    Check ((Digest 'first.ppm') -eq $saved) 'Failed atomic publication changed previous output'
    # Cancellation is allowed to race completion; accepted always means no publication.
    $cancelled = Capture $server $state2 'cancel.ppm'
    $cancel = Value (Call-Rpc $server 'jobs.cancel' @{id=$cancelled.job_id})
    $cancelJob = Wait-Job $server $cancelled.job_id
    if ($cancel.accepted) {
        Check ($cancelJob.state -eq 'cancelled') 'Accepted cancel reported success'
        Check (-not (Test-Path -LiteralPath (Join-Path $root 'cancel.ppm'))) 'Cancelled task published an image'
    } else { Check ($cancelJob.state -eq 'succeeded') 'Terminal cancellation was not idempotent' }
    [void](Value (Call-Rpc $server 'runtime.shutdown'))
    Finish $server
} finally { Dispose-Server $server }
# EOF and shutdown both join active GPU work; no implicit Scene write.
foreach ($mode in @('eof','shutdown')) {
    $server = $null
    try {
        $server = Start-Server
        $state = Value (Call-Rpc $server 'scene.load' @{manifest='project.json'})
        [void](Capture $server $state ($mode+'.ppm'))
        if ($mode -eq 'eof') { $server.Writer.Dispose() } else { [void](Value (Call-Rpc $server 'runtime.shutdown')) }
        Finish $server
        Check ((Digest 'scene.json') -eq $diskScene) 'Shutdown persisted Scene'
    } finally { Dispose-Server $server }
}
Write-Output ('M7.4 passed: pinned revision/frame, unsaved transform, idle publication, failure preservation, cancellation and GPU shutdown. Artifacts: '+$root)
