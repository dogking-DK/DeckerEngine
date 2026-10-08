param(
    [Parameter(Mandatory=$true)][string]$Editor,
    [Parameter(Mandatory=$true)][string]$Fixtures,
    [Parameter(Mandatory=$true)][string]$WorkDir
)
$ErrorActionPreference = 'Stop'
# Unique retained evidence; never run the mutating smoke against the real demo.
$caseDir = Join-Path $WorkDir ("editor-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $caseDir | Out-Null
Copy-Item -LiteralPath $Fixtures -Destination (Join-Path $caseDir 'project') -Recurse
$projectDir = Join-Path $caseDir 'project'
Set-Content -LiteralPath (Join-Path $projectDir '.dk-editor-smoke') -Value 'disposable UI acceptance fixture'
$image = Join-Path $caseDir 'workbench.ppm'
& $Editor --root $projectDir --manifest project.json --validation --smoke --fixture-camera --screenshot $image
$code = $LASTEXITCODE
if ($code -eq 77) { exit 77 }
if ($code -ne 0) { throw "Editor smoke failed with exit $code; evidence: $caseDir" }
if (!(Test-Path -LiteralPath $image) -or (Get-Item -LiteralPath $image).Length -lt 100000) {
    throw "Missing workbench image: $image"
}
$project = Get-Content -LiteralPath (Join-Path $projectDir 'project.json') -Raw | ConvertFrom-Json
$scene = Get-Content -LiteralPath (Join-Path $projectDir $project.scene) -Raw | ConvertFrom-Json
if (@($scene.entities | Where-Object { $_.components.'dk.Name'.value -eq 'Workbench smoke entity' -and $_.components.'dk.Transform'.translation[0] -eq 0.25 }).Count -ne 1) {
    throw 'UI save did not persist exactly one renamed entity'
}
Write-Output "Editor smoke persistence and screenshot passed: $caseDir"