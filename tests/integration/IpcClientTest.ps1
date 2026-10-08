param(
    [Parameter(Mandatory=$true)][string]$Runner,
    [Parameter(Mandatory=$true)][string]$Client,
    [Parameter(Mandatory=$true)][string]$WorkDir,
    [string]$Editor = '',
    [string]$Fixtures = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$utf8 = [System.Text.UTF8Encoding]::new($false)
$root = Join-Path $WorkDir ('ipc-' + [char]0x573a + [char]0x666f + ' space-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $root | Out-Null
$endpoint = 'process-' + [guid]::NewGuid().ToString('N')
$script:serial = 0
$server = $null
function Check([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Launch([string]$File, [string[]]$Arguments) {
    $info = [System.Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $File
    # All JSON is passed through files; Windows paths cannot contain quotation marks.
    $info.Arguments = ($Arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $info.UseShellExecute = $false; $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = $utf8; $info.StandardErrorEncoding = $utf8
    $process = [System.Diagnostics.Process]::new(); $process.StartInfo = $info
    [void]$process.Start()
    return [pscustomobject]@{ Process=$process; Output=$process.StandardOutput.ReadToEndAsync(); Errors=$process.StandardError.ReadToEndAsync() }
}
function Dispose-Process($Item) {
    if ($null -eq $Item) { return }
    if (-not $Item.Process.HasExited) { $Item.Process.Kill(); [void]$Item.Process.WaitForExit(5000) }
    $Item.Process.Dispose()
}
function Ctl([string[]]$Arguments, [int]$Exit = 0) {
    $item = Launch $Client (@('--pipe',$endpoint) + $Arguments)
    try {
        Check ($item.Process.WaitForExit(65000)) 'dk-ctl hung'
        Check ($item.Output.Wait(1000) -and $item.Errors.Wait(1000)) 'dk-ctl streams did not close'
        Check ($item.Process.ExitCode -eq $Exit) ("dk-ctl exit $($item.Process.ExitCode), expected $Exit : " + $item.Output.Result + $item.Errors.Result)
        Check ($item.Errors.Result -eq '') 'dk-ctl leaked diagnostics to stderr'
        $lines = @($item.Output.Result.TrimEnd() -split "`n")
        Check ($lines.Count -eq 1) 'dk-ctl stdout must contain exactly one JSON result'
        return ($lines[0] | ConvertFrom-Json)
    } finally { Dispose-Process $item }
}
function Call([string]$Method, [hashtable]$Params = @{}, [int]$Exit = 0, $Retry = $null) {
    $script:serial++
    $file = Join-Path $root ('params-' + [char]0x53c2 + [char]0x6570 + "-$script:serial.json")
    [System.IO.File]::WriteAllText($file,($Params | ConvertTo-Json -Depth 32 -Compress),$utf8)
    $arguments = @('--method',$Method,'--params-file',$file)
    if ($null -ne $Retry) { $arguments += @('--session',$Retry.session,'--request-id',[string]$Retry.request_id) }
    return Ctl $arguments $Exit
}
function Value($Reply) { return $Reply.response.result.value }
function State { return (Value (Call 'scene.query')).state }
function Guard($State) { return @{document_id=$State.document_id;revision=$State.revision} }
function Start-Server {
    if ($Editor) { return Launch $Editor @('--root',$root,'--pipe',$endpoint,'--validation','--fixture-camera') }
    return Launch $Runner @('--project-root',$root,'--pipe',$endpoint)
}
function Finish {
    [void](Call 'runtime.shutdown')
    Check ($server.Process.WaitForExit(10000)) 'Host failed to stop after delivering shutdown reply'
    Check ($server.Process.ExitCode -eq 0) ('Host failed: ' + $server.Errors.Result)
    Check ($server.Output.Wait(1000) -and $server.Errors.Wait(1000)) 'Host streams did not close'
    [System.IO.File]::WriteAllText((Join-Path $root 'host.stdout.log'),$server.Output.Result,$utf8)
    [System.IO.File]::WriteAllText((Join-Path $root 'host.stderr.log'),$server.Errors.Result,$utf8)
    if ($Editor) {
        Check ($server.Output.Result -match 'validation errors=0 warnings=0 liveAllocations=0') 'Editor validation or lifetime failure'
    } else { Check ($server.Output.Result -eq '' -and $server.Errors.Result -eq '') 'Runner pipe output leaked to console' }
}
try {
    if ($Editor) { Copy-Item -Path (Join-Path $Fixtures '*') -Destination $root -Recurse }
    $server = Start-Server
    # Connect/hello has an absolute deadline and needs no stdin to make progress.
    $hello = Ctl @('--hello','--timeout-ms','60000')
    Check ($hello.hello.protocol -eq 1 -and $hello.hello.limits.request_bytes -eq 1048576) 'Wrong IPC discovery'
    if (-not $Editor) { [void](Call 'scene.new') }
    $before = State
    $id = [guid]::NewGuid().ToString()
    $params = @{id=$id;guard=(Guard $before)}
    $created = Call 'entity.create' $params
    $replayed = Call 'entity.create' $params 0 $created
    Check ($created.response.result.task_id -eq $replayed.response.result.task_id) 'Retry created a second task'
    $after = State
    Check ($after.entity_count -eq $before.entity_count+1 -and $after.revision -eq $before.revision+1) 'Mutation was missing or repeated'
    $conflict = Call 'entity.create' @{id=([guid]::NewGuid().ToString());guard=(Guard $before)} 1 $created
    Check ($conflict.response.error.code -eq -32072) 'Different request reused a ticket'
    $stale = Call 'entity.set_name' @{id=$id;name='stale';guard=(Guard $before)} 1
    Check ($stale.response.error.code -eq -32007) 'IPC implicitly replaced stale guard'
    $unicodeName = [string][char]0x6d4b + [char]0x8bd5 + ' pipe entity'
    [void](Call 'entity.set_name' @{id=$id;name=$unicodeName;guard=(Guard $after)})
    [void](Call 'history.undo' @{guard=(Guard (State))})
    [void](Call 'history.redo' @{guard=(Guard (State))})
    [void](Call 'scene.save' @{guard=(Guard (State))})
    if (-not $Editor) { [void](Call 'project.save' @{manifest='project.json';guard=(Guard (State))}) }
    $query = Value (Call 'scene.query')
    Check (($query.entities | Where-Object id -eq $id).name -eq $unicodeName) 'UTF-8 parameters were corrupted'
    $unknown = Call 'missing.method' @{} 1
    Check ($unknown.response.error.code -eq -32601) 'Wrong command error exit/result'
    [void](Ctl @('--method','scene.query','--auto-guard') 2)
    [void](Ctl @('--hello','--timeout-ms','0') 2)
    [void](Ctl @('--method','scene.query','--session','invalid','--request-id','1') 2)
    $bom = Join-Path $root 'bom.json'
    [System.IO.File]::WriteAllText($bom,'{}',[System.Text.UTF8Encoding]::new($true))
    [void](Ctl @('--method','scene.query','--params-file',$bom) 2)
    Finish
    Dispose-Process $server; $server = $null
    if (-not $Editor) {
        $server = Start-Server
        [void](Ctl @('--hello'))
        $retry = Call 'entity.create' $params 3 $created
        Check ($retry.execution -eq 'not_sent' -and $retry.status -eq 'invalid') 'Old session request was resent'
        [void](Call 'scene.load' @{manifest='project.json'})
        $loaded = Value (Call 'scene.query')
        Check (($loaded.entities | Where-Object id -eq $id).name -eq $unicodeName) 'Saved result did not survive restart'
        Finish
    }
    Write-Output "IPC process checks passed: $root"
} finally { Dispose-Process $server }
