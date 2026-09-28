#requires -Version 5.1
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$shell = (Get-Process -Id $PID).Path
$runRoot = Join-Path $repoRoot ('out/check-spec-tests/' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
$utf8 = [Text.UTF8Encoding]::new($false)
New-Item -ItemType Directory -Path $runRoot -Force | Out-Null

function Write-Fixture([string]$Root, [string]$File, [string]$Body) {
    $destination = Join-Path $Root $File
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    [IO.File]::WriteAllText($destination, $Body, $utf8)
}
function Replace-Fixture([string]$Root, [string]$File, [string]$Old, [string]$New) {
    $body = Get-Content -LiteralPath (Join-Path $Root $File) -Raw -Encoding UTF8
    if (-not $body.Contains($Old)) { throw "Fixture replacement not found: $File -> $Old" }
    Write-Fixture $Root $File $body.Replace($Old, $New)
}

$baseline = @{
    'README.md' = @'
# Fixture
[design](spec/design/sample.md#not-validated)
`[inline example](missing-inline.md)`
```markdown
[example](missing-code.md)
| detached | example |
```
~~~markdown
[example](missing-tilde.md)
~~~

| Name | Value |
| --- | --- |
| expression | `a\|b` |
'@
    'AGENTS.md' = '# Fixture instructions'
    'spec/templates/design.md' = 'Template without metadata'
    'spec/design/sample.md' = @'
---
module: sample
created_at: "2026-09-28T10:00:00+08:00"
updated_at: "2026-09-28T11:00:00+08:00"
status: accepted
---
# Sample
'@
    'spec/development/0001-sample.md' = @'
---
id: "0001"
created_at: "2026-09-28T10:00:00+08:00"
updated_at: "2026-09-28T11:00:00+08:00"
status: completed
design_refs:
  - ../design/sample.md
---
# Sample work
'@
    'spec/design/README.md' = @'
| Module | Document | Status | Scope |
| --- | --- | --- | --- |
| sample | [Sample](sample.md) | accepted | fixture |
'@
    'spec/development/README.md' = @'
| ID | Document | Modules | Status |
| --- | --- | --- | --- |
| 0001 | [Sample](0001-sample.md) | sample | completed (documentation only) |
'@
    '.agents/skills/decker-build-verify/references/test-selection.md' = @'
| Behavior | Target | Filter |
| --- | --- | --- |
| fixture | dk_fixture | `^dk.fixture.` |
'@
    'tests/unit/CMakeLists.txt' = "add_executable(dk_fixture main.cpp)`n# add_executable(dk_commented ignored.cpp)"
    'CMakePresets.json' = '{"version":6}'
    'vcpkg.json' = '{"name":"fixture"}'
}
$cases = @(
    @{ name = 'valid'; change = {}; error = '' },
    @{ name = 'limited-scan'; change = {}; error = ''; limited = $true },
    @{ name = 'empty-table-cell'; change = { param($r) Replace-Fixture $r 'README.md' '| expression | `a\|b` |' '| expression | |' }; error = '' },
    @{ name = 'broken-link'; change = { param($r) Write-Fixture $r 'README.md' '[missing](missing.md)' }; error = 'Broken local link' },
    @{ name = 'metadata-outside-frontmatter'; change = { param($r) Replace-Fixture $r 'spec/design/sample.md' 'module: sample' '# module: sample' }; error = 'Missing or duplicate module' },
    @{ name = 'invalid-design-status'; change = { param($r) Replace-Fixture $r 'spec/design/sample.md' 'status: accepted' 'status: completed' }; error = 'Invalid design status' },
    @{ name = 'invalid-development-status'; change = { param($r) Replace-Fixture $r 'spec/development/0001-sample.md' 'status: completed' 'status: accepted' }; error = 'Invalid development status' },
    @{ name = 'missing-index-entry'; change = { param($r) Replace-Fixture $r 'spec/design/README.md' '| sample | [Sample](sample.md) | accepted | fixture |' '' }; error = 'Document missing from design index'; limited = $true },
    @{ name = 'index-status-mismatch'; change = { param($r) Replace-Fixture $r 'spec/development/README.md' 'completed (documentation only)' 'blocked' }; error = 'Index metadata mismatch' },
    @{ name = 'index-key-mismatch'; change = { param($r) Replace-Fixture $r 'spec/design/README.md' '| sample |' '| other |' }; error = 'Index metadata mismatch' },
    @{ name = 'duplicate-index-entry'; change = { param($r) $line = '| sample | [Sample](sample.md) | accepted | fixture |'; Replace-Fixture $r 'spec/design/README.md' $line ($line + "`n" + $line) }; error = 'Duplicate document in design index' },
    @{ name = 'unknown-index-document'; change = { param($r) Replace-Fixture $r 'spec/design/README.md' '(sample.md)' '(missing.md)' }; error = 'Unknown document in design index'; limited = $true },
    @{ name = 'missing-design-reference'; change = { param($r) Replace-Fixture $r 'spec/development/0001-sample.md' '../design/sample.md' '../design/missing.md' }; error = 'Invalid design_refs target'; limited = $true },
    @{ name = 'wrong-reference-directory'; change = { param($r) Replace-Fixture $r 'spec/development/0001-sample.md' '../design/sample.md' '../../README.md' }; error = 'Invalid design_refs target' },
    @{ name = 'missing-reference-list'; change = { param($r) Replace-Fixture $r 'spec/development/0001-sample.md' 'design_refs:' 'other_refs:' }; error = 'Missing design_refs list' },
    @{ name = 'duplicate-development-id'; change = { param($r) Write-Fixture $r 'spec/development/0001-other.md' $baseline['spec/development/0001-sample.md'] }; error = 'Duplicate id' },
    @{ name = 'id-filename-mismatch'; change = { param($r) Replace-Fixture $r 'spec/development/0001-sample.md' 'id: "0001"' 'id: "0002"' }; error = 'Development ID differs' },
    @{ name = 'timestamp-order'; change = { param($r) Replace-Fixture $r 'spec/design/sample.md' '11:00:00' '09:00:00' }; error = 'Invalid timestamp order' },
    @{ name = 'timestamp-timezone'; change = { param($r) Replace-Fixture $r 'spec/design/sample.md' '+08:00' '' }; error = 'Invalid ISO timestamp' },
    @{ name = 'detached-table'; change = { param($r) Replace-Fixture $r 'spec/design/README.md' '| sample |' "`n| sample |" }; error = 'Detached table row' },
    @{ name = 'table-width'; change = { param($r) Replace-Fixture $r 'README.md' '| expression |' '| expression | extra |' }; error = 'Table column count differs' },
    @{ name = 'unclosed-fence'; change = { param($r) Write-Fixture $r 'README.md' '```markdown' }; error = 'Unclosed Markdown code fence' },
    @{ name = 'unknown-test-target'; change = { param($r) Replace-Fixture $r '.agents/skills/decker-build-verify/references/test-selection.md' 'dk_fixture' 'dk_missing' }; error = 'Unknown test target' },
    @{ name = 'missing-test-entry'; change = { param($r) Write-Fixture $r 'tests/integration/CMakeLists.txt' 'add_executable(dk_new_probe probe.cpp)' }; error = 'Test target missing from selection table'; limited = $true },
    @{ name = 'invalid-json'; change = { param($r) Write-Fixture $r 'vcpkg.json' '{' }; error = 'JSON' }
)
$results = @()
try {
    foreach ($case in $cases) {
        $caseRoot = Join-Path $runRoot $case.name
        foreach ($file in $baseline.Keys) { Write-Fixture $caseRoot $file $baseline[$file] }
        Write-Fixture $caseRoot 'scripts/check-spec.ps1' (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'check-spec.ps1') -Raw -Encoding UTF8)
        & $case.change $caseRoot
        $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $caseRoot 'scripts/check-spec.ps1'))
        if ($case.limited) { $arguments += @('-Path', 'README.md') }
        $log = Join-Path $caseRoot 'result.log'
        $ErrorActionPreference = 'Continue'
        & $shell @arguments *> $log
        $code = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        $output = Get-Content -LiteralPath $log -Raw
        $passed = if ($case.error) { $code -ne 0 -and $output -match [regex]::Escape($case.error) } else { $code -eq 0 }
        $results += [pscustomobject]@{ name = $case.name; passed = $passed; exit_code = $code; expected_error = $case.error }
        if (-not $passed) { throw "Case $($case.name) failed expectation (exit $code). See $log`n$output" }
        Write-Host "PASS $($case.name)"
    }
} finally {
    $summary = [ordered]@{ shell = $shell; version = $PSVersionTable.PSVersion.ToString(); cases = $results }
    [IO.File]::WriteAllText((Join-Path $runRoot 'summary.json'), ($summary | ConvertTo-Json -Depth 5), $utf8)
    Write-Host "Evidence: $runRoot"
}
Write-Host "Passed $($results.Count) document-check cases. No engine build was run."
