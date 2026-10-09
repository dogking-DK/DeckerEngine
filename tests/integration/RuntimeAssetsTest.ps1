param([Parameter(Mandatory=$true)][string]$Runner,
      [Parameter(Mandatory=$true)][string]$Fixtures,
      [Parameter(Mandatory=$true)][string]$WorkDir)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Join-Path $WorkDir ('ra-' + [guid]::NewGuid().ToString('N').Substring(0,16) + ' space')
$assetDir = Join-Path $root 'assets'
New-Item -ItemType Directory -Force -Path $assetDir | Out-Null
foreach ($name in @('triangle.gltf','triangle.bin','rgba.png')) {
    Copy-Item -LiteralPath (Join-Path $Fixtures $name) -Destination (Join-Path $assetDir $name)
}
$utf8 = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllText((Join-Path $root 'project.json'), '{"format":"DeckerProject","version":1,"name":"CPU","scene":"scene.json","assets":[]}', $utf8)
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
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
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
$server = $null
try {
    $server = Start-Server
    $caps = Value (Call-Rpc $server 'runtime.capabilities')
    Check ($caps.async_jobs -and -not $caps.async_tasks) 'Async capability/TaskId compatibility mismatch'
    $commands = @(Value (Call-Rpc $server 'commands.list'))
    Check ($commands.Count -eq $(if ($caps.render_capture) { 41 } else { 40 })) 'Unexpected command discovery count'
    foreach ($name in @('assets.open','assets.catalog','assets.import','assets.register','assets.rename','assets.load','assets.status','assets.unload','jobs.get','jobs.wait','jobs.cancel')) {
        $description = Value (Call-Rpc $server 'commands.describe' @{name=$name})
        Check (-not $description.undoable -and -not $description.parameters.additionalProperties) ('Bad command contract: '+$name)
    }
    $opened = Value (Call-Rpc $server 'assets.open' @{manifest='project.json'})
    $registered = Value (Call-Rpc $server 'assets.register' @{guard=$opened.guard;source='assets/triangle.gltf';create_meta=$true})
    $initialMeta = Get-Content -LiteralPath (Join-Path $assetDir 'triangle.gltf.meta') -Raw | ConvertFrom-Json
    $submitted = Value (Call-Rpc $server 'assets.import' @{source='assets/triangle.gltf'})
    # Deliberately send no requests: persistence must advance while stdin stays open.
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    $metaPath = Join-Path $assetDir 'triangle.gltf.meta'
    $currentPath = $null
    while ([DateTime]::UtcNow -lt $deadline) {
        if (Test-Path -LiteralPath $metaPath) {
            $metadata = Get-Content -LiteralPath $metaPath -Raw | ConvertFrom-Json
            $currentPath = Join-Path $root ('.decker/cache/assets/v1/current/'+$metadata.root_id+'.json')
            if (Test-Path -LiteralPath $currentPath) { break }
        }
        [System.Threading.Thread]::Sleep(10)
    }
    if ($null -eq $currentPath -or -not (Test-Path -LiteralPath $currentPath)) {
        $failure = Call-Rpc $server 'jobs.get' @{id=$submitted.job_id}
        throw ('Completion did not publish while idle: '+($failure | ConvertTo-Json -Depth 32 -Compress))
    }
    $job = Value (Call-Rpc $server 'jobs.get' @{id=$submitted.job_id})
    Check ($job.state -eq 'succeeded' -and $job.result.root_id -eq $metadata.root_id) 'Job reported inconsistent publication'
    Check ($job.result.root_id -eq $initialMeta.root_id) 'Import replaced registered identity'
    $rootId = $job.result.root_id
    $firstKey = $job.result.key
    $identities = @($metadata.outputs | ForEach-Object { $_.id } | Sort-Object)
    $registered = Value (Call-Rpc $server 'assets.register' @{guard=$registered.guard;source='assets/triangle.gltf'})
    Check ($registered.total -eq 3) 'Imported mesh/material/texture outputs not registered'
    $oldGuard = $registered.guard
    $load = Value (Call-Rpc $server 'assets.load' @{id=$rootId})
    Check ((Wait-Job $server $load.job_id).state -eq 'succeeded') 'Initial load failed'
    $ready = Value (Call-Rpc $server 'assets.status' @{id=$rootId})
    Check ($ready.state -eq 'ready' -and $ready.artifact.cache_hit) 'CPU data did not become Ready through cache'
    $bufferPath = Join-Path $assetDir 'triangle.bin'
    $bytes = [System.IO.File]::ReadAllBytes($bufferPath)
    $mtime = [System.IO.File]::GetLastWriteTimeUtc($bufferPath)
    [BitConverter]::GetBytes([single]2).CopyTo($bytes,0)
    [System.IO.File]::WriteAllBytes($bufferPath,$bytes)
    [System.IO.File]::SetLastWriteTimeUtc($bufferPath,$mtime)
    $rebuild = Value (Call-Rpc $server 'assets.import' @{source='assets/triangle.gltf'})
    $rebuilt = Wait-Job $server $rebuild.job_id
    Check ($rebuilt.state -eq 'succeeded' -and $rebuilt.result.root_id -eq $rootId -and $rebuilt.result.key -ne $firstKey) 'Dependency content change did not rebuild stable identities'
    $metadata = Get-Content -LiteralPath $metaPath -Raw | ConvertFrom-Json
    Check ((@($metadata.outputs | ForEach-Object { $_.id } | Sort-Object) -join ',') -eq ($identities -join ',')) 'Subasset identities changed'
    # A recoverable worker error must leave the previous current index intact.
    $savedCurrent = [System.IO.File]::ReadAllText($currentPath)
    Remove-Item -LiteralPath $bufferPath
    $bad = Value (Call-Rpc $server 'assets.import' @{source='assets/triangle.gltf'})
    Check ((Wait-Job $server $bad.job_id).state -eq 'failed') 'Missing dependency was not a failed Job'
    Check ([System.IO.File]::ReadAllText($currentPath) -eq $savedCurrent) 'Failed job replaced current'
    [System.IO.File]::WriteAllBytes($bufferPath,$bytes)
    $retry = Value (Call-Rpc $server 'assets.load' @{id=$rootId})
    Check ((Wait-Job $server $retry.job_id).state -eq 'succeeded') 'Failed asset was not retryable'
    $cancelled = Value (Call-Rpc $server 'assets.import' @{source='assets/triangle.gltf';unit_scale=3})
    $cancel = Value (Call-Rpc $server 'jobs.cancel' @{id=$cancelled.job_id})
    $cancelResult = Wait-Job $server $cancelled.job_id
    if ($cancel.accepted) {
        Check ($cancelResult.state -eq 'cancelled') 'Accepted cancellation published a terminal success'
        Check ([System.IO.File]::ReadAllText($currentPath) -eq $savedCurrent) 'Accepted cancellation changed current'
        Check ((Value (Call-Rpc $server 'assets.status' @{id=$rootId})).state -eq 'unloaded') 'Cancelled generation stayed Ready'
    } else { Check ($cancelResult.state -eq 'succeeded') 'Terminal cancellation was not idempotent' }
    $unloaded = Value (Call-Rpc $server 'assets.unload' @{id=$rootId})
    Check ($unloaded.state -eq 'unloaded' -and $null -eq $unloaded.artifact) 'Unload retained current owning value'
    # New Scene save into the same active catalog must preserve registered mappings.
    $state = Value (Call-Rpc $server 'scene.new' @{name='CPU'})
    $state = Value (Call-Rpc $server 'scene.save' @{guard=(Scene-Guard $state)})
    $state = Value (Call-Rpc $server 'project.save' @{guard=(Scene-Guard $state);manifest='project.json'})
    $created = Value (Call-Rpc $server 'entity.create' @{guard=(Scene-Guard $state)})
    $entityId = $created.created_id; $state = $created.state
    $edit = Value (Call-Rpc $server 'entity.set_assets' @{guard=(Scene-Guard $state);id=$entityId;assets=@(@{id=$rootId;kind='mesh'})})
    $state = Value (Call-Rpc $server 'scene.save' @{guard=(Scene-Guard $edit.state)})
    $savedScene = [System.IO.File]::ReadAllText((Join-Path $root 'scene.json'))
    [void](Value (Call-Rpc $server 'entity.set_name' @{guard=(Scene-Guard $state);id=$entityId;name='unsaved'}))
    $shutdown = Value (Call-Rpc $server 'runtime.shutdown')
    Check ($shutdown.stopping) 'Shutdown response was not flushed'
    Finish $server
    Check ([System.IO.File]::ReadAllText((Join-Path $root 'scene.json')) -eq $savedScene) 'Shutdown implicitly saved Scene'
} finally { Dispose-Server $server }
$server = $null
try {
    $server = Start-Server
    $catalog = Value (Call-Rpc $server 'assets.open' @{manifest='project.json'})
    Check ($catalog.guard.session_id -ne $oldGuard.session_id -and $catalog.total -eq 3) 'Restart did not separate session and persistent identities'
    $oldJob = Call-Rpc $server 'jobs.get' @{id=$submitted.job_id}
    Check ($oldJob.error.code -eq -32003) 'Restart retained an old JobId'
    $stale = Call-Rpc $server 'assets.register' @{guard=$oldGuard;source='assets/triangle.gltf'}
    Check ($stale.error.code -eq -32007) 'Restart accepted an old catalog guard'
    $load = Value (Call-Rpc $server 'assets.load' @{id=$rootId})
    Check ((Wait-Job $server $load.job_id).state -eq 'succeeded') 'Restart could not reload CPU asset'
    $ready = Value (Call-Rpc $server 'assets.status' @{id=$rootId})
    Check ($ready.state -eq 'ready' -and $ready.artifact.cache_hit) 'Restart failed to reuse verified cache'
    [void](Value (Call-Rpc $server 'scene.load' @{manifest='project.json'}))
    $entity = Value (Call-Rpc $server 'entity.get' @{id=$entityId})
    Check ($entity.assets[0].id -eq $rootId -and $entity.name -ne 'unsaved') 'Persistent Scene reference or unsaved-state isolation failed'
    # EOF with outstanding work and a partial final line must be drained and joined.
    [void](Value (Call-Rpc $server 'assets.import' @{source='assets/triangle.gltf';unit_scale=4}))
    $server.Writer.Write('{"jsonrpc":"2.0","method":"runtime.capabilities"}')
    $server.Writer.Dispose()
    Finish $server
} finally { Dispose-Server $server }
# Shutdown while the reader is receiving more lines than its bounded queue can hold.
$server = $null
try {
    $server = Start-Server
    [void](Value (Call-Rpc $server 'assets.import' @{source='assets/triangle.gltf';unit_scale=5}))
    $lines = @('{"jsonrpc":"2.0","id":"stop","method":"runtime.shutdown"}')
    1..32 | ForEach-Object { $lines += '{"jsonrpc":"2.0","method":"runtime.capabilities"}' }
    $server.Writer.WriteLine(($lines -join "`n"))
    $response = $server.Process.StandardOutput.ReadLineAsync()
    Check ($response.Wait(10000)) 'Shutdown did not flush with a queued reader'
    $stop = $response.Result | ConvertFrom-Json
    Check ($stop.id -eq 'stop' -and $stop.result.value.stopping) 'Wrong shutdown acknowledgement'
    Finish $server
} finally { Dispose-Server $server }
Write-Output 'M4 CPU delivery verified: registered stable IDs, idle-input publication, Ready, invalidation, failure recovery, cancellation, Scene isolation, restart, EOF and busy shutdown.'
