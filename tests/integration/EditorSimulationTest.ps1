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
& $Editor --background --root $projectDir --manifest project.json --validation --simulation-smoke --fixture-camera --screenshot $image
$code = $LASTEXITCODE
if ($code -eq 77) { exit 77 }
if ($code -ne 0) { throw "Simulation panel smoke failed: $caseDir" }
if (!(Test-Path -LiteralPath $image) -or (Get-Item -LiteralPath $image).Length -lt 100000) { throw 'Missing simulation panel screenshot' }
if ((Get-FileHash -LiteralPath (Join-Path $projectDir 'scene.json')).Hash -ne $before) { throw 'Simulation changed scene file' }
$exports = @(Get-ChildItem -LiteralPath $projectDir -Directory -Filter 'experiment-*')
if ($exports.Count -ne 2) { throw 'Expected one CPU and one GPU export' }
$solvers = @()
foreach ($experiment in $exports) {
    $config = Get-Content -LiteralPath (Join-Path $experiment.FullName 'config.json') -Raw | ConvertFrom-Json
    $metrics = Get-Content -LiteralPath (Join-Path $experiment.FullName 'metrics.json') -Raw | ConvertFrom-Json
    $provenance = Get-Content -LiteralPath (Join-Path $experiment.FullName 'provenance.json') -Raw | ConvertFrom-Json
    $particles = @(Get-Content -LiteralPath (Join-Path $experiment.FullName 'particles.json') -Raw | ConvertFrom-Json)
    if ($experiment.Name -ne "experiment-$($provenance.run_id)-1" -or $config.steps -ne 1 -or $metrics.steps -ne 1 -or $provenance.steps -ne 1 -or
        $metrics.simulated_time_ns -ne 10000000 -or $provenance.simulated_time_ns -ne 10000000 -or $particles.Count -ne 64 -or
        $metrics.metrics.particle_count -ne 64 -or $provenance.performance.steps -ne 1 -or $provenance.performance.batch_steps -ne 1 -or
        $provenance.performance.wall_time_ns -le 0 -or $config.view.width -ne $provenance.width -or $config.view.height -ne $provenance.height) {
        throw "Export identity/steps/metrics differ: $($experiment.FullName)"
    }
    if ((Get-Item -LiteralPath (Join-Path $experiment.FullName 'image.ppm')).Length -lt 640*480*3) { throw 'Incomplete image' }
    $solvers += $config.solver
}
if ((($solvers | Sort-Object) -join ',') -ne 'xpbd_cpu,xpbd_gpu') { throw 'Wrong export backends' }
if (@(Get-ChildItem -LiteralPath $projectDir -Directory -Filter '.dk-experiment-*').Count) { throw 'Leaked staging directory' }
Write-Output "Simulation CPU/GPU metrics, stale samples, exact export, visible failure/no-overwrite, viewport and scene isolation passed: $caseDir"
