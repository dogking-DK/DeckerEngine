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
    Check ($commands.Count -eq 33) 'Unexpected command discovery count'
    foreach ($name in @('assets.open','assets.catalog','assets.import','assets.register','assets.rename','assets.load','assets.status','assets.unload','jobs.get','jobs.wait','jobs.cancel')) {
        $description = Value (Call-Rpc $server 'commands.describe' @{name=$name})
        Check (-not $description.undoable -and -not $description.parameters.additionalProperties) ('Bad command contract: '+$name)
    }
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
    $shutdown = Value (Call-Rpc $server 'runtime.shutdown')
    Check ($shutdown.stopping) 'Shutdown response was not flushed'
    Finish $server
} finally { Dispose-Server $server }
Write-Output 'Assets stdio verified: discovery/schema, independent import, idle-input completion publication, clean shutdown.'
