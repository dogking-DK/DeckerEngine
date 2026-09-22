param([Parameter(Mandatory=$true)][string]$Runner, [Parameter(Mandatory=$true)][string]$WorkDir)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$script:requestId = 0
$rootName = 'stdio-' + [char]0x573a + [char]0x666f + ' space-' + [guid]::NewGuid().ToString()
$root = Join-Path $WorkDir $rootName
New-Item -ItemType Directory -Force -Path $root | Out-Null
$utf8 = [System.Text.UTF8Encoding]::new($false)
function Check([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Start-Server {
    $info = [System.Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $Runner
    $info.Arguments = '--project-root "' + $root + '" --stdio'
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = $utf8
    $info.StandardErrorEncoding = $utf8
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $info
    [void]$process.Start()
    $writer = [System.IO.StreamWriter]::new($process.StandardInput.BaseStream, $utf8)
    $writer.AutoFlush = $true
    return [pscustomobject]@{ Process = $process; Writer = $writer; Errors = $process.StandardError.ReadToEndAsync() }
}
function Read-Response($Server) {
    $pending = $Server.Process.StandardOutput.ReadLineAsync()
    Check ($pending.Wait(10000)) 'Response not flushed while stdin remains open'
    $line = $pending.Result
    Check ($null -ne $line -and $line.Length -gt 0) 'Unexpected EOF or empty protocol output'
    return ($line | ConvertFrom-Json)
}
function Call-Rpc($Server, [string]$Method, [hashtable]$Parameters = @{}) {
    $script:requestId += 1
    $request = @{ jsonrpc = '2.0'; id = $script:requestId; method = $Method; params = $Parameters }
    $Server.Writer.WriteLine(($request | ConvertTo-Json -Depth 32 -Compress))
    $response = Read-Response $Server
    Check ($response.jsonrpc -eq '2.0' -and $response.id -eq $script:requestId) 'Wrong response ID or notification response'
    return $response
}
function Value($Response) {
    Check ($Response.PSObject.Properties.Name -contains 'result') ('Expected success: ' + ($Response | ConvertTo-Json -Depth 32 -Compress))
    Check ($Response.result.status -eq 'succeeded') 'Missing terminal success status'
    $parsedId = [guid]::Empty
    Check ([guid]::TryParse($Response.result.task_id, [ref]$parsedId)) 'Missing task UUID'
    return $Response.result.value
}
function Guard($State) { return @{ document_id = $State.document_id; revision = $State.revision } }
function Finish($Server) {
    Check ($Server.Process.WaitForExit(10000)) 'Server did not exit promptly'
    Check ($Server.Process.ExitCode -eq 0) 'Recoverable stdio error changed the exit code'
    Check ($Server.Errors.Wait(2000)) 'stderr did not close'
    Check ($Server.Errors.Result -eq '') ('Protocol diagnostics leaked to stderr: ' + $Server.Errors.Result)
    Check ($Server.Process.StandardOutput.ReadToEnd() -eq '') 'Unexpected trailing protocol output'
}
function Dispose-Server($Server) {
    if ($null -eq $Server) { return }
    if (-not $Server.Process.HasExited) { $Server.Process.Kill(); [void]$Server.Process.WaitForExit(3000) }
    $Server.Writer.Dispose()
    $Server.Process.Dispose()
}
$server = $null
try {
    $server = Start-Server
    $caps = Value (Call-Rpc $server 'runtime.capabilities')
    Check (-not $caps.async_tasks -and $caps.task_retention -eq 256) 'Incorrect synchronous capabilities'
    $state = Value (Call-Rpc $server 'scene.new')
    $oldGuard = Guard $state
    $parent = '11111111-1111-4111-8111-111111111111'
    $child = '22222222-2222-4222-8222-222222222222'
    $childName = [string][char]0x5B50 + [char]0x8282 + [char]0x70B9
    $steps = @(
        @{ method='entity.create'; params=@{id=$parent} },
        @{ method='entity.create'; params=@{id=$child} },
        @{ method='entity.set_parent'; params=@{id=$child; parent=$parent} },
        @{ method='entity.set_name'; params=@{id=$child; name=$childName} },
        @{ method='entity.set_transform'; params=@{id=$parent; transform=@{translation=@(3,2,1); rotation=@(0,0,0,1); scale=@(2,1,1)}} }
    )
    $transaction = Value (Call-Rpc $server 'scene.transaction' @{guard=$oldGuard; commands=$steps})
    $state = $transaction.state
    Check ($state.revision -eq 1 -and $state.entity_count -eq 2) 'Transaction was not one revision'
    $conflict = Call-Rpc $server 'entity.create' @{guard=$oldGuard}
    Check ($conflict.error.code -eq -32007 -and $conflict.error.data.status -eq 'failed') 'Stale guard was not rejected'
    $task = Value (Call-Rpc $server 'tasks.get' @{id=$conflict.error.data.task_id})
    Check ($task.status -eq 'failed' -and $task.error_code -eq 7) 'Failed task metadata was not retained'
    $before = Value (Call-Rpc $server 'scene.query')
    $expectedEntities = $before.entities | ConvertTo-Json -Depth 32 -Compress
    Check ($before.entities[1].name -eq $childName -and $before.entities[1].world_matrix[3] -eq 3) 'Unicode or hierarchy transform failed'
    $state = Value (Call-Rpc $server 'history.undo' @{guard=(Guard $state)})
    Check ($state.entity_count -eq 0 -and $state.revision -eq 2) 'Undo did not restore the empty scene'
    $state = Value (Call-Rpc $server 'history.redo' @{guard=(Guard $state)})
    Check ($state.entity_count -eq 2 -and $state.revision -eq 3) 'Redo lost identities or revision'
    $server.Writer.WriteLine('{"jsonrpc":"2.0","method":"commands.list"}')
    $entity = Value (Call-Rpc $server 'entity.get' @{id=$child})
    Check ($entity.id -eq $child) 'Notification disturbed the next response'
    $server.Writer.WriteLine('{"jsonrpc":"2.0","id":1,"id":2,"method":"commands.list"}')
    $parseError = Read-Response $server
    Check ($parseError.error.code -eq -32700 -and $null -eq $parseError.id) 'Duplicate keys not rejected'
    $server.Writer.WriteLine('[{"jsonrpc":"2.0","id":"batch-ok","method":"runtime.capabilities"},{"jsonrpc":"2.0","method":"commands.list"},{"jsonrpc":"2.0","id":"batch-bad","method":"missing"}]')
    $batch = @(Read-Response $server)
    Check ($batch.Count -eq 2 -and $batch[0].id -eq 'batch-ok' -and $batch[1].error.code -eq -32601) 'Mixed batch contract failed'
    $state = Value (Call-Rpc $server 'scene.save' @{guard=(Guard $state)})
    Check (-not $state.dirty) 'Save did not establish a clean baseline'
    $state = Value (Call-Rpc $server 'project.save' @{guard=(Guard $state); manifest='project.json'})
    $stopping = Value (Call-Rpc $server 'runtime.shutdown')
    Check ($stopping.stopping) 'Shutdown did not acknowledge before exit'
    Finish $server
} finally { Dispose-Server $server }
$server = $null
try {
    $server = Start-Server
    $state = Value (Call-Rpc $server 'scene.load' @{manifest='project.json'})
    Check (-not $state.dirty -and $state.revision -eq 3) 'Restart did not load the saved baseline'
    $loaded = Value (Call-Rpc $server 'scene.query')
    Check (($loaded.entities | ConvertTo-Json -Depth 32 -Compress) -eq $expectedEntities) 'Persistent entity values changed across restart'
    $bad = Call-Rpc $server 'missing'
    Check ($bad.error.code -eq -32601) 'Error recovery after restart failed'
    $server.Writer.Dispose()
    Finish $server
} finally { Dispose-Server $server }
Write-Output 'Persistent stdio verified: immediate responses, transactions, errors, tasks, shutdown, EOF and restart.'
