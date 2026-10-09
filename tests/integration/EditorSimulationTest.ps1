param(
    [Parameter(Mandatory=$true)][string]$Editor,
    [Parameter(Mandatory=$true)][string]$Fixtures,
    [Parameter(Mandatory=$true)][string]$WorkDir
)
$ErrorActionPreference = 'Stop'
$caseDir = Join-Path $WorkDir ('editor-simulation-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $caseDir | Out-Null
Copy-Item -LiteralPath $Fixtures -Destination (Join-Path $caseDir 'project') -Recurse
$projectDir = Join-Path $caseDir 'project'
Set-Content -LiteralPath (Join-Path $projectDir '.dk-editor-smoke') -Value 'disposable simulation panel fixture'
$image = Join-Path $caseDir 'simulation-panel.ppm'
$before = (Get-FileHash -LiteralPath (Join-Path $projectDir 'scene.json')).Hash
& $Editor --root $projectDir --manifest project.json --validation --simulation-smoke --fixture-camera --screenshot $image
$code = $LASTEXITCODE
if ($code -eq 77) { exit 77 }
if ($code -ne 0) { throw "Simulation panel smoke failed: $caseDir" }
if (!(Test-Path -LiteralPath $image) -or (Get-Item -LiteralPath $image).Length -lt 100000) { throw 'Missing simulation panel screenshot' }
if ((Get-FileHash -LiteralPath (Join-Path $projectDir 'scene.json')).Hash -ne $before) { throw 'Simulation changed scene file' }
Write-Output "Simulation panel CPU/GPU buttons, state and scene isolation passed: $caseDir"
