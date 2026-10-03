param(
    [string]$Runner = "$PSScriptRoot/../../out/build/windows-graphics/bin/Debug/dk-run.exe",
    [string]$ProjectRoot = "$PSScriptRoot/../../projects/demo",
    [string]$Manifest = 'project.json',
    [string]$Output = 'captures/sponza.ppm',
    [int]$Width = 960,
    [int]$Height = 540,
    [switch]$RequireValidation
)
# Minimal stdio example: use returned guards and JobIds, never hard-code session IDs.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = [System.IO.Path]::GetFullPath($ProjectRoot)
$destination = [System.IO.Path]::GetFullPath($Output,$root)
$prefix = $root.TrimEnd([System.IO.Path]::DirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
if (-not $destination.StartsWith($prefix,[System.StringComparison]::OrdinalIgnoreCase) -or [System.IO.Path]::GetExtension($destination) -ne '.ppm') {
    throw 'Output must be a .ppm file inside ProjectRoot'
}
if (-not (Test-Path -LiteralPath $root -PathType Container)) { throw 'ProjectRoot must exist' }
New-Item -ItemType Directory -Force -Path ([System.IO.Path]::GetDirectoryName($destination)) | Out-Null
$utf8 = [System.Text.UTF8Encoding]::new($false)
$info = [System.Diagnostics.ProcessStartInfo]::new()
$info.FileName = [System.IO.Path]::GetFullPath($Runner)
$info.UseShellExecute = $false; $info.CreateNoWindow = $true
$info.ArgumentList.Add('--project-root'); $info.ArgumentList.Add($root); $info.ArgumentList.Add('--stdio')
$info.RedirectStandardInput = $true; $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
$info.StandardOutputEncoding = $utf8; $info.StandardErrorEncoding = $utf8
$process = [System.Diagnostics.Process]::new(); $process.StartInfo = $info
$writer = $null
$started = $false
try {
    [void]$process.Start(); $started = $true
    $diagnostics = $process.StandardError.ReadToEndAsync()
    $writer = [System.IO.StreamWriter]::new($process.StandardInput.BaseStream,$utf8); $writer.AutoFlush = $true
    $script:rpcId = 0
    function Invoke-Rpc([string]$method,$parameters=@{}) {
        $script:rpcId++
        $writer.WriteLine((@{jsonrpc='2.0';id=$script:rpcId;method=$method;params=$parameters} | ConvertTo-Json -Depth 16 -Compress))
        $read = $process.StandardOutput.ReadLineAsync()
        if (-not $read.Wait(10000) -or $null -eq $read.Result) { throw ('No response: '+$method) }
        $reply = $read.Result | ConvertFrom-Json
        if ($reply.id -ne $script:rpcId) { throw 'Response ID mismatch' }
        if ($reply.PSObject.Properties.Name -contains 'error') { throw ($reply.error | ConvertTo-Json -Depth 16 -Compress) }
        return $reply.result.value
    }
    $capabilities = Invoke-Rpc 'runtime.capabilities'
    if (-not $capabilities.render_capture) { throw 'Build dk-run with DK_BUILD_RENDER_CAPTURE=ON' }
    $state = Invoke-Rpc 'scene.load' @{manifest=$Manifest}
    $ticket = Invoke-Rpc 'render.capture' @{guard=@{document_id=$state.document_id;revision=$state.revision};
        output=$Output;width=$Width;height=$Height;validation=$(if ($RequireValidation) { 'required' } else { 'if_available' })}
    $deadline = [DateTime]::UtcNow.AddSeconds(120)
    do {
        $waited = Invoke-Rpc 'jobs.wait' @{id=$ticket.job_id;timeout_ms=1000}
        if (-not $waited.timed_out) { break }
        if ([DateTime]::UtcNow -ge $deadline) {
            [void](Invoke-Rpc 'jobs.cancel' @{id=$ticket.job_id})
            throw 'Capture deadline exceeded; cancellation requested'
        }
    } while ($true)
    if ($waited.job.state -ne 'succeeded') { throw ($waited.job | ConvertTo-Json -Depth 16 -Compress) }
    [void](Invoke-Rpc 'runtime.shutdown')
    if (-not $process.WaitForExit(30000)) { throw 'Runner did not finish shutdown' }
    $stderr = $diagnostics.GetAwaiter().GetResult()
    if ($stderr) { [Console]::Error.Write($stderr) }
    if ($process.ExitCode -ne 0) { throw ('Runner exit code: '+$process.ExitCode) }
    $waited.job.result | ConvertTo-Json -Depth 8
} finally {
    if ($writer) { $writer.Dispose() }
    if ($started -and -not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
    $process.Dispose()
}
