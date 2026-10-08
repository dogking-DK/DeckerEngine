param(
    [Parameter(Mandatory=$true)][string]$Editor,
    [Parameter(Mandatory=$true)][string]$Runner,
    [Parameter(Mandatory=$true)][string]$Client,
    [Parameter(Mandatory=$true)][string]$Fixtures,
    [Parameter(Mandatory=$true)][string]$WorkDir
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$utf8 = [System.Text.UTF8Encoding]::new($false)
$caseDir = Join-Path $WorkDir ('editor-consistency-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $caseDir -Force | Out-Null
$root = Join-Path $caseDir 'project'
Copy-Item -LiteralPath $Fixtures -Destination $root -Recurse
[IO.File]::WriteAllText((Join-Path $root '.dk-editor-smoke'),'disposable consistency fixture',$utf8)
$evidence = Join-Path $root '.dk-consistency'
$endpoint = 'consistency-' + [guid]::NewGuid().ToString('N')
$script:serial = 0
$editorProcess = $null; $runnerProcess = $null
$checks = [Collections.Generic.List[object]]::new()
function Check([bool]$Ok,[string]$Message) { if (-not $Ok) { throw $Message } }
function Launch([string]$File,[string[]]$Arguments) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $File
    $info.Arguments = ($Arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $info.UseShellExecute = $false; $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = $utf8; $info.StandardErrorEncoding = $utf8
    $process = [Diagnostics.Process]::new(); $process.StartInfo = $info; [void]$process.Start()
    return [pscustomobject]@{Process=$process;Output=$process.StandardOutput.ReadToEndAsync();Errors=$process.StandardError.ReadToEndAsync()}
}
function Dispose-Process($Item,[string]$Name) {
    if ($null -eq $Item) { return }
    if (-not $Item.Process.HasExited) { $Item.Process.Kill(); [void]$Item.Process.WaitForExit(5000) }
    if ($Item.Output.Wait(1000)) { [IO.File]::WriteAllText((Join-Path $caseDir "$Name.stdout.log"),$Item.Output.Result,$utf8) }
    if ($Item.Errors.Wait(1000)) { [IO.File]::WriteAllText((Join-Path $caseDir "$Name.stderr.log"),$Item.Errors.Result,$utf8) }
    $Item.Process.Dispose()
}
function Call([string]$Method,[hashtable]$Params=@{},[int]$Exit=0,$Retry=$null) {
    $script:serial++
    $file = Join-Path $caseDir "params-$script:serial.json"
    [IO.File]::WriteAllText($file,($Params | ConvertTo-Json -Depth 32 -Compress),$utf8)
    $arguments = @('--pipe',$endpoint,'--method',$Method,'--params-file',$file,'--timeout-ms','10000')
    if ($null -ne $Retry) { $arguments += @('--session',$Retry.session,'--request-id',[string]$Retry.request_id) }
    $item = Launch $Client $arguments
    try {
        Check ($item.Process.WaitForExit(15000)) "dk-ctl hung: $Method"
        Check ($item.Output.Wait(1000) -and $item.Errors.Wait(1000)) 'Client streams did not close'
        [IO.File]::WriteAllText((Join-Path $caseDir "reply-$script:serial.json"),$item.Output.Result,$utf8)
        Check ($item.Process.ExitCode -eq $Exit) ("dk-ctl $Method returned $($item.Process.ExitCode), expected $Exit : " + $item.Output.Result + $item.Errors.Result)
        Check ($item.Errors.Result -eq '') 'Client leaked stderr diagnostics'
        $reply = $item.Output.Result | ConvertFrom-Json
        Check ($reply.execution -eq 'received') "No valid RPC reply: $Method"
        return $reply
    } finally { Dispose-Process $item "ctl-$script:serial" }
}
function Value($Reply) { return $Reply.response.result.value }
function Guard($State) { return @{document_id=$State.document_id;revision=$State.revision} }
function Digest([string]$Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function Next([string]$Name) { [IO.File]::WriteAllText((Join-Path $evidence "$Name.continue"),'continue',$utf8) }
function Checkpoint([string]$Name) {
    $path = Join-Path $evidence "$Name.json"
    $deadline = [DateTime]::UtcNow.AddSeconds(45)
    while (-not (Test-Path -LiteralPath $path)) {
        if ($editorProcess.Process.HasExited) { throw ('Editor exited before '+$Name+': '+$editorProcess.Errors.Result) }
        Check ([DateTime]::UtcNow -lt $deadline) "Checkpoint timeout: $Name"
        Start-Sleep -Milliseconds 20
    }
    $report = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    Check ($report.checkpoint -eq $Name -and -not $report.viewport.preview) 'Checkpoint did not describe committed pixels'
    Check ($report.viewport.document_id -eq $report.state.document_id -and $report.viewport.scene_id -eq $report.state.scene_id -and
        $report.viewport.revision -eq $report.state.revision) 'Published viewport does not match GUI revision'
    return $report
}
function Compare-Entities($Expected,$Actual) {
    Check (@($Expected).Count -eq @($Actual).Count) 'Entity count differs across entry points'
    foreach ($e in $Expected) {
        $matches = @($Actual | Where-Object id -eq $e.id)
        Check ($matches.Count -eq 1) "Missing entity $($e.id)"
        $a = $matches[0]
        Check ($e.name -ceq $a.name -and $e.parent -eq $a.parent) 'Name or parent differs'
        foreach ($field in @('translation','rotation','scale')) {
            Check ($e.transform.$field.Count -eq $a.transform.$field.Count) 'TRS length mismatch'
            for ($i=0; $i -lt $e.transform.$field.Count; ++$i) {
                Check ([Math]::Abs($e.transform.$field[$i]-$a.transform.$field[$i]) -lt 1e-12) "TRS mismatch: $field"
            }
        }
        Check (($e.assets | ConvertTo-Json -Depth 8 -Compress) -ceq ($a.assets | ConvertTo-Json -Depth 8 -Compress)) 'Asset references differ'
    }
}
function Compare-State($Report,[bool]$SameDocument=$true) {
    $query = Value (Call 'scene.query' @{limit=256})
    Check (-not $query.has_more) 'Fixture comparison was accidentally paginated'
    foreach ($field in @('scene_id','revision','entity_count','dirty')) {
        Check ($query.state.$field -eq $Report.state.$field) "GUI and RPC disagree on $field"
    }
    if ($SameDocument) { Check ($query.state.document_id -eq $Report.state.document_id) 'GUI and RPC disagree on session' }
    else { Check ($query.state.document_id -ne $Report.state.document_id) 'Restart retained old document session' }
    Compare-Entities $Report.entities $query.entities
    if ($null -ne $Report.selection -and -not $Report.pending) {
        Compare-Entities @($Report.inspector) @($query.entities | Where-Object id -eq $Report.selection)
    }
    return $query.state
}
function Submit-Capture($Report,[string]$Output,$State=$null) {
    if ($null -eq $State) { $State = $Report.state }
    return Value (Call 'render.capture' @{guard=(Guard $State);output=$Output;width=$Report.viewport.width;height=$Report.viewport.height;
        camera=$Report.viewport.camera;profile='unlit_preview';validation='required'})
}
function Wait-Capture($Ticket) {
    $deadline = [DateTime]::UtcNow.AddSeconds(45)
    do {
        $wait = Value (Call 'jobs.wait' @{id=$Ticket.job_id;timeout_ms=1000})
        if (-not $wait.timed_out) { break }
        Check ([DateTime]::UtcNow -lt $deadline) 'Capture job timed out'
    } while ($true)
    Check ($wait.job.state -eq 'succeeded') ('Capture failed: '+($wait.job | ConvertTo-Json -Depth 16 -Compress))
    foreach ($field in @('document_id','scene_id','revision','frame','width','height','output')) {
        Check ($wait.job.result.$field -eq $Ticket.$field) "Capture completion changed submitted $field"
    }
    Check ($wait.job.result.draw_count -eq 3) 'Capture lost fixture draws'
    return $wait.job.result
}
function Compare-Capture($Report,[string]$Name,$State=$null) {
    $ticket = Submit-Capture $Report ".dk-consistency/$Name.ppm" $State
    $result = Wait-Capture $ticket
    $expected = Digest (Join-Path $root $Report.viewport.image)
    $actual = Digest (Join-Path $root $result.output)
    Check ($actual -eq $expected) "Viewport and capture pixels differ: $Name"
    $checks.Add([ordered]@{name=$Name;scene_id=$result.scene_id;document_id=$result.document_id;revision=$result.revision;frame=$result.frame;
        width=$result.width;height=$result.height;sha256=$actual})
}
try {
    $diskBefore = Digest (Join-Path $root 'scene.json')
    $editorProcess = Launch $Editor @('--root',$root,'--pipe',$endpoint,'--validation','--consistency-smoke','--fixture-camera',
        '--screenshot',(Join-Path $caseDir 'workbench.ppm'))
    $initial = Checkpoint 'initial'; [void](Compare-State $initial); Compare-Capture $initial 'capture-initial'; Next 'initial'
    $gui = Checkpoint 'gui'; [void](Compare-State $gui); Compare-Capture $gui 'capture-gui'
    Check ((Digest (Join-Path $root 'scene.json')) -eq $diskBefore) 'GUI Apply/capture implicitly saved'
    # Capture the old version, then mutate without waiting for capture completion.
    $frozen = Submit-Capture $gui '.dk-consistency/capture-frozen.ppm'
    $id = $gui.selection
    $transaction = @{guard=(Guard $gui.state);commands=@(
        @{method='entity.set_name';params=@{id=$id;name='External consistency'}},
        @{method='entity.set_transform';params=@{id=$id;transform=@{translation=@(0.6,-0.125,0.25);rotation=@(0,0,0,1);scale=@(0.5,0.5,1)}}}
    )}
    $remote = Call 'scene.transaction' $transaction
    $replay = Call 'scene.transaction' $transaction 0 $remote
    Check ($replay.response.result.task_id -eq $remote.response.result.task_id) 'IPC retry allocated another mutation TaskId'
    $stale = Call 'entity.set_name' @{guard=(Guard $gui.state);id=$id;name='forbidden stale write'} 1
    Check ($stale.response.error.code -eq -32007) 'Old revision write was accepted'
    $rejected = Call 'render.capture' @{guard=(Guard $gui.state);output='.dk-consistency/rejected.ppm'} 1
    Check ($rejected.response.error.code -eq -32007 -and -not (Test-Path (Join-Path $evidence 'rejected.ppm'))) 'Stale capture published output'
    $frozenResult = Wait-Capture $frozen
    Check ($frozenResult.revision -eq $gui.state.revision -and (Digest (Join-Path $root $frozenResult.output)) -eq
        (Digest (Join-Path $root $gui.viewport.image))) 'Accepted old capture observed newer edits'
    Next 'gui'
    $external = Checkpoint 'external'; [void](Compare-State $external); Compare-Capture $external 'capture-external'; Next 'external'
    $draft = Checkpoint 'draft'; [void](Compare-State $draft); Compare-Capture $draft 'capture-draft'
    Check ($draft.pending -and $draft.inspector.name -eq 'Stale GUI draft') 'GUI draft was not exercised'
    [void](Call 'scene.transaction' @{guard=(Guard $draft.state);commands=@(
        @{method='entity.set_name';params=@{id=$id;name='External during draft'}},
        @{method='entity.set_transform';params=@{id=$id;transform=@{translation=@(-0.35,-0.125,0.25);rotation=@(0,0,0,1);scale=@(0.5,0.5,1)}}}
    )})
    Next 'draft'
    foreach ($stage in @('conflict','undo','redo')) {
        $report = Checkpoint $stage; [void](Compare-State $report); Compare-Capture $report "capture-$stage"
        Check ((Digest (Join-Path $root 'scene.json')) -eq $diskBefore) 'Edit/history/capture implicitly changed Scene file'
        Next $stage
    }
    $saved = Checkpoint 'saved'; [void](Compare-State $saved); Compare-Capture $saved 'capture-saved'
    Check (-not $saved.state.dirty -and (Digest (Join-Path $root 'scene.json')) -ne $diskBefore) 'GUI Save did not publish Scene'
    Next 'saved'
    $reloaded = Checkpoint 'reloaded'; [void](Compare-State $reloaded); Compare-Capture $reloaded 'capture-reloaded'
    Compare-Entities $saved.entities $reloaded.entities
    $oldSession = Call 'entity.set_name' @{guard=(Guard $saved.state);id=$id;name='forbidden ABA write'} 1
    Check ($oldSession.response.error.code -eq -32007) 'Reload accepted old document guard'
    Next 'reloaded'
    Check ($editorProcess.Process.WaitForExit(15000)) 'Editor did not finish acceptance run'
    Check ($editorProcess.Process.ExitCode -eq 0) ('Editor failure: '+$editorProcess.Errors.Result)
    Check ($editorProcess.Output.Wait(1000) -and $editorProcess.Errors.Wait(1000)) 'Editor output did not close'
    Check ($editorProcess.Errors.Result -eq '') 'Editor/capture emitted validation diagnostics'
    Check ($editorProcess.Output.Result -match 'validation errors=0 warnings=0 liveAllocations=0') 'Editor lifetime/validation failed'
    Check ((Get-Item -LiteralPath (Join-Path $caseDir 'workbench.ppm')).Length -gt 100000) 'Workbench screenshot missing'

    $endpoint = 'reload-' + [guid]::NewGuid().ToString('N')
    $runnerProcess = Launch $Runner @('--project-root',$root,'--pipe',$endpoint)
    [void](Call 'scene.load' @{manifest='project.json'})
    $runnerState = Compare-State $reloaded $false
    Compare-Capture $reloaded 'capture-runner' $runnerState
    [void](Call 'runtime.shutdown')
    Check ($runnerProcess.Process.WaitForExit(15000)) 'Runner did not stop'
    Check ($runnerProcess.Process.ExitCode -eq 0) 'Runner failed'
    Check ($runnerProcess.Output.Wait(1000) -and $runnerProcess.Errors.Wait(1000)) 'Runner output did not close'
    Check ($runnerProcess.Output.Result -eq '' -and $runnerProcess.Errors.Result -eq '') 'Runner/capture emitted diagnostics'
    [IO.File]::WriteAllText((Join-Path $caseDir 'acceptance.json'),(@{status='passed';checks=$checks.ToArray();frozen=$frozenResult;
        saved_revision=$saved.state.revision;editor_document=$reloaded.state.document_id;runner_document=$runnerState.document_id} | ConvertTo-Json -Depth 16),$utf8)
    Write-Output "M8.4 GUI, IPC, capture and runner consistency passed: $caseDir"
} finally { Dispose-Process $editorProcess 'editor'; Dispose-Process $runnerProcess 'runner' }
