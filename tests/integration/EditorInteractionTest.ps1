param(
    [Parameter(Mandatory=$true)][string]$Editor,
    [Parameter(Mandatory=$true)][string]$Runner,
    [Parameter(Mandatory=$true)][string]$Fixtures,
    [Parameter(Mandatory=$true)][string]$WorkDir
)
$ErrorActionPreference = 'Stop'
$caseDir = Join-Path $WorkDir ('editor-interaction-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $caseDir | Out-Null
Copy-Item -LiteralPath $Fixtures -Destination (Join-Path $caseDir 'project') -Recurse
$projectDir = Join-Path $caseDir 'project'
Set-Content -LiteralPath (Join-Path $projectDir '.dk-editor-smoke') -Value 'disposable interaction fixture'
$image = Join-Path $caseDir 'interaction.ppm'
& $Editor --root $projectDir --validation --interaction-smoke --fixture-camera --screenshot $image
$code = $LASTEXITCODE
if ($code -eq 77) { exit 77 }
if ($code -ne 0) { throw "Editor interaction failed with exit $code; evidence: $caseDir" }
if (!(Test-Path -LiteralPath $image) -or (Get-Item -LiteralPath $image).Length -lt 100000) { throw 'Missing interaction screenshot' }
$scene = Get-Content -LiteralPath (Join-Path $projectDir 'scene.json') -Raw | ConvertFrom-Json
$edited = @($scene.entities | Where-Object { $_.components.'dk.Name'.value -eq 'mask' })
if ($edited.Count -ne 1) { throw 'Missing picked entity' }
$transform = $edited[0].components.'dk.Transform'
if ([Math]::Abs($transform.translation[0]) -lt 0.001 -or [Math]::Abs($transform.rotation[2]) -lt 0.001 -or [Math]::Abs($transform.scale[0] - 0.5) -lt 0.001) {
    throw 'Move/Rotate/Scale were not persisted'
}
$batch = Join-Path $caseDir 'reload.jsonl'
@'
{"jsonrpc":"2.0","id":1,"method":"scene.load","params":{"manifest":"project.json"}}
{"jsonrpc":"2.0","id":2,"method":"scene.query","params":{"limit":128}}
'@ | Set-Content -LiteralPath $batch -Encoding utf8NoBOM
$output = & $Runner --project-root $projectDir --batch $batch
if ($LASTEXITCODE -ne 0) { throw 'Runner reload failed' }
$output | Set-Content -LiteralPath (Join-Path $caseDir 'runner.jsonl')
$responses = @($output | ForEach-Object { $_ | ConvertFrom-Json })
$response = @($responses | Where-Object { $_.id -eq 2 })[0]
if ($response.error -or $response.result.status -ne 'succeeded') { throw 'Runner query failed' }
$query = $response.result.value
if ($null -eq $query.entities) { throw 'Runner query did not return entities' }
if ($query.state.revision -ne $scene.revision) { throw 'Runner loaded a different revision' }
foreach ($entity in $scene.entities) {
    $id = $entity.components.'dk.Identity'.id
    $actual = @($query.entities | Where-Object { $_.id -eq $id })
    if ($actual.Count -ne 1) { throw "Runner lost entity $id" }
    foreach ($field in @('translation','rotation','scale')) {
        $expected = $entity.components.'dk.Transform'.$field
        $values = $actual[0].transform.$field
        if ($values.Count -ne $expected.Count) { throw "Runner $field length mismatch" }
        for ($i = 0; $i -lt $values.Count; ++$i) {
            if ([Math]::Abs($values[$i] - $expected[$i]) -gt 1e-10) { throw "Runner transform mismatch: $id $field" }
        }
    }
}
Write-Output "Interaction, cancellation, persistence and runner reload passed: $caseDir"
