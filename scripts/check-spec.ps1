#requires -Version 5.1
[CmdletBinding()]
param([string[]]$Path = @())

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if ($Path.Count) {
    $documents = @($Path | ForEach-Object {
        $candidate = $_
        if (-not [System.IO.Path]::IsPathRooted($candidate)) { $candidate = Join-Path $repoRoot $candidate }
        $item = Get-Item -LiteralPath $candidate
        if ($item.PSIsContainer -or $item.Extension -ne '.md') { throw "Expected a Markdown file: $candidate" }
        $item
    })
} else {
    $documents = @(Get-Item -LiteralPath (Join-Path $repoRoot 'README.md'), (Join-Path $repoRoot 'AGENTS.md'))
    $documents += @(Get-ChildItem -LiteralPath (Join-Path $repoRoot 'spec') -Recurse -Filter '*.md')
    $documents += @(Get-ChildItem -LiteralPath (Join-Path $repoRoot '.agents/skills') -Recurse -Filter '*.md')
}
$linkCount = 0
foreach ($document in $documents) {
    $body = Get-Content -LiteralPath $document.FullName -Raw -Encoding UTF8
    foreach ($match in [regex]::Matches($body, '\[[^\]]*\]\(([^)]+)\)')) {
        $target = $match.Groups[1].Value
        if ($target -match '^[a-zA-Z][a-zA-Z0-9+.-]*:' -or $target.StartsWith('#')) { continue }
        $target = ($target -split '#')[0]
        if (-not (Test-Path -LiteralPath (Join-Path $document.DirectoryName $target))) {
            throw "Broken local link: $($document.FullName) -> $target"
        }
        $linkCount++
    }
    $relative = $document.FullName.Substring($repoRoot.Length + 1).Replace('\', '/')
    if ($relative -match '^spec/(design|development|commands)/' -and $document.Name -ne 'README.md' -or
        ($relative -like 'spec/*' -and $body.StartsWith('---') -and $relative -notlike 'spec/templates/*')) {
        $created = [regex]::Match($body, '(?m)^created_at: "([^"]+)"').Groups[1].Value
        $updated = [regex]::Match($body, '(?m)^updated_at: "([^"]+)"').Groups[1].Value
        if (-not $created -or -not $updated) { throw "Missing timestamps: $relative" }
        foreach ($stamp in @($created, $updated)) {
            if ($stamp -notmatch '(Z|[+-]\d{2}:\d{2})$') { throw "Missing timestamp timezone: $relative" }
        }
        if ([DateTimeOffset]::Parse($updated) -lt [DateTimeOffset]::Parse($created)) { throw "Invalid timestamp order: $relative" }
    }
}
foreach ($file in @('CMakePresets.json', 'vcpkg.json')) {
    Get-Content -LiteralPath (Join-Path $repoRoot $file) -Raw -Encoding UTF8 | ConvertFrom-Json | Out-Null
}
$ids = @(Get-ChildItem -LiteralPath (Join-Path $repoRoot 'spec/development') -Filter '*.md' | Where-Object { $_.Name -match '^\d+-' } | ForEach-Object {
    $id = ($_.BaseName -split '-', 2)[0]
    $body = Get-Content -LiteralPath $_.FullName -Raw -Encoding UTF8
    $declared = [regex]::Match($body, '(?m)^id: "([^"]+)"').Groups[1].Value
    if ($declared -ne $id) { throw "Development ID differs from filename: $($_.Name)" }
    $id
})
if (($ids | Select-Object -Unique).Count -ne $ids.Count) { throw 'Duplicate development IDs' }
Write-Host "Validated $($documents.Count) Markdown files, $linkCount local links, spec timestamps, development IDs and JSON manifests."
