param(
    [Parameter(Mandatory=$true)][string]$Editor,
    [Parameter(Mandatory=$true)][string]$Fixtures,
    [Parameter(Mandatory=$true)][string]$WorkDir
)
$ErrorActionPreference = 'Stop'
$caseDir = Join-Path $WorkDir ('editor-settings-help-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $caseDir | Out-Null
Copy-Item -LiteralPath $Fixtures -Destination (Join-Path $caseDir 'project') -Recurse
$projectDir = Join-Path $caseDir 'project'
Set-Content -LiteralPath (Join-Path $projectDir '.dk-editor-smoke') -Value 'disposable settings help fixture'
$scene = Join-Path $projectDir 'scene.json'
$before = (Get-FileHash -LiteralPath $scene).Hash
$image = Join-Path $caseDir 'settings-help.ppm'
$log = Join-Path $caseDir 'editor.log'
& $Editor --background --root $projectDir --validation --settings-help-smoke --fixture-camera --screenshot $image *> $log
$code = $LASTEXITCODE
Get-Content -LiteralPath $log
if ($code -ne 0) { throw "Editor settings help failed with exit $code; evidence: $caseDir" }
if (!(Test-Path -LiteralPath $image) -or (Get-Item -LiteralPath $image).Length -lt 100000) { throw 'Missing settings screenshot' }
if ((Get-FileHash -LiteralPath $scene).Hash -ne $before) { throw 'Help changed the scene file' }
if (!(Select-String -LiteralPath $log -Pattern 'Settings help smoke passed:' -Quiet)) { throw 'Missing native UI acceptance result' }
Write-Output "Settings help and unchanged scene file verified: $caseDir"
