#requires -Version 5.1
[CmdletBinding()]
param([string[]]$Path = @())

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

function Read-Utf8([string]$File) {
    Get-Content -LiteralPath $File -Raw -Encoding UTF8
}

# Retain line numbers while excluding fenced examples from structural checks.
function Get-Prose([string]$Body) {
    $fenceChar = ''; $fenceLength = 0
    $lines = foreach ($line in ($Body -split '\r?\n')) {
        $marker = [regex]::Match($line, '^ {0,3}(`{3,}|~{3,})(.*)$')
        if ($fenceChar) {
            if ($marker.Success -and $marker.Groups[1].Value[0].ToString() -eq $fenceChar -and
                $marker.Groups[1].Length -ge $fenceLength -and -not $marker.Groups[2].Value.Trim()) {
                $fenceChar = ''; $fenceLength = 0
            }
            ''
        } elseif ($marker.Success) {
            $fenceChar = $marker.Groups[1].Value[0].ToString(); $fenceLength = $marker.Groups[1].Length
            ''
        } else { $line }
    }
    if ($fenceChar) { throw 'Unclosed Markdown code fence' }
    $lines -join "`n"
}

function Get-Cells([string]$Line) {
    # Literal pipes must be escaped in table cells, including inline code.
    $row = $Line.Trim()
    @([regex]::Split($row.Substring(1, $row.Length - 2), '(?<!\\)\|') | ForEach-Object { $_.Trim() })
}

function Test-Tables([string]$Prose, [string]$File) {
    $lines = $Prose -split '\n'; $width = 0
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i].Trim()
        if (-not ($line.StartsWith('|') -and $line.EndsWith('|'))) { $width = 0; continue }
        $cells = @(Get-Cells $line)
        if (-not $width) {
            if ($i + 1 -ge $lines.Count -or $lines[$i + 1].Trim() -notmatch '^\|(?:\s*:?-{3,}:?\s*\|)+$') {
                throw "Detached table row or missing separator: ${File}:$($i + 1)"
            }
            $width = $cells.Count
        }
        if ($cells.Count -ne $width) { throw "Table column count differs: ${File}:$($i + 1)" }
    }
}

function Get-Frontmatter([string]$Body, [string]$File) {
    $match = [regex]::Match($Body, '\A---\r?\n(.*?)\r?\n---(?:\r?\n|\z)', 'Singleline')
    if (-not $match.Success) { throw "Missing frontmatter: $File" }
    $match.Groups[1].Value
}

function Get-Field([string]$Metadata, [string]$Name, [string]$File) {
    $matches = [regex]::Matches($Metadata, ('(?m)^' + [regex]::Escape($Name) + ':[ \t]*(.+?)[ \t]*\r?$'))
    if ($matches.Count -ne 1) { throw "Missing or duplicate ${Name}: $File" }
    $matches[0].Groups[1].Value.Trim().Trim('"').Trim("'")
}

function Test-Timestamps([string]$Metadata, [string]$File) {
    $created = Get-Field $Metadata 'created_at' $File
    $updated = Get-Field $Metadata 'updated_at' $File
    foreach ($stamp in @($created, $updated)) {
        if ($stamp -notmatch '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?(?:Z|[+-]\d{2}:\d{2})$') {
            throw "Invalid ISO timestamp or missing timezone: $File"
        }
    }
    if ([DateTimeOffset]::Parse($updated) -lt [DateTimeOffset]::Parse($created)) { throw "Invalid timestamp order: $File" }
}

function Test-Catalog([string]$Kind) {
    $directory = Join-Path $repoRoot "spec/$Kind"
    $documents = @(Get-ChildItem -LiteralPath $directory -Filter '*.md' | Where-Object { $_.Name -ne 'README.md' })
    $keyField = 'module'; $allowed = @('draft', 'accepted', 'superseded')
    if ($Kind -eq 'development') { $keyField = 'id'; $allowed = @('in-progress', 'completed', 'blocked') }
    $catalog = @{}; $keys = @{}
    foreach ($document in $documents) {
        $metadata = Get-Frontmatter (Read-Utf8 $document.FullName) $document.Name
        Test-Timestamps $metadata $document.Name
        $key = Get-Field $metadata $keyField $document.Name
        $status = Get-Field $metadata 'status' $document.Name
        if ($allowed -notcontains $status) { throw "Invalid ${Kind} status: $($document.Name)" }
        if ($keys.ContainsKey($key)) { throw "Duplicate ${keyField}: $key" }
        $keys[$key] = $true
        if ($Kind -eq 'design' -and $key -ne $document.BaseName) { throw "Module differs from filename: $($document.Name)" }
        if ($Kind -eq 'development') {
            if ($document.Name -notmatch '^\d{4,}-' -or ($document.BaseName -split '-', 2)[0] -ne $key) {
                throw "Development ID differs from filename: $($document.Name)"
            }
            $refs = [regex]::Match($metadata, '(?m)^design_refs:[ \t]*\r?\n((?:[ \t]+-[^\r\n]*(?:\r?\n|$))+)')
            if (-not $refs.Success) { throw "Missing design_refs list: $($document.Name)" }
            foreach ($ref in [regex]::Matches($refs.Groups[1].Value, '(?m)^[ \t]+-[ \t]+([^\r\n]+)')) {
                $target = $ref.Groups[1].Value.Trim().Trim('"').Trim("'")
                $resolved = [IO.Path]::GetFullPath((Join-Path $directory $target))
                if ([IO.Path]::GetDirectoryName($resolved) -ne (Join-Path $repoRoot 'spec/design') -or
                    [IO.Path]::GetExtension($resolved) -ne '.md' -or [IO.Path]::GetFileName($resolved) -eq 'README.md' -or
                    -not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
                    throw "Invalid design_refs target: $($document.Name) -> $target"
                }
            }
        }
        $catalog[$document.Name] = @{ key = $key; status = $status }
    }
    $indexFile = Join-Path $directory 'README.md'
    $prose = Get-Prose (Read-Utf8 $indexFile)
    Test-Tables $prose $indexFile
    $seen = @{}
    $indexLines = $prose -split '\n'
    for ($row = 0; $row -lt $indexLines.Count; $row++) {
        $line = $indexLines[$row].Trim()
        if (-not $line.StartsWith('|') -or $line -match '^\|(?:\s*:?-{3,}:?\s*\|)+$') { continue }
        if ($row + 1 -lt $indexLines.Count -and $indexLines[$row + 1].Trim() -match '^\|(?:\s*:?-{3,}:?\s*\|)+$') { continue }
        $cells = @(Get-Cells $line)
        $link = [regex]::Match($cells[1], '^\[[^\]]+\]\(([^)]+\.md)\)$')
        if (-not $link.Success -or -not $catalog.ContainsKey($link.Groups[1].Value)) { throw "Unknown document in ${Kind} index: $line" }
        $file = $link.Groups[1].Value
        if ($seen.ContainsKey($file)) { throw "Duplicate document in ${Kind} index: $file" }
        $seen[$file] = $true
        $statusColumn = 2
        if ($Kind -eq 'development') { $statusColumn = 3 }
        # Allow a parenthesized note after an index status.
        $indexStatus = [regex]::Match($cells[$statusColumn], '^([a-z-]+)(?:$|\s|\(|\uFF08)').Groups[1].Value
        if ($cells[0] -ne $catalog[$file].key -or $indexStatus -ne $catalog[$file].status) { throw "Index metadata mismatch: $file" }
    }
    foreach ($file in $catalog.Keys) {
        if (-not $seen.ContainsKey($file)) { throw "Document missing from ${Kind} index: $file" }
    }
}

function Test-TestEntrypoints {
    $selectionFile = Join-Path $repoRoot '.agents/skills/decker-build-verify/references/test-selection.md'
    $prose = Get-Prose (Read-Utf8 $selectionFile)
    Test-Tables $prose $selectionFile
    $documented = @{}
    foreach ($line in ($prose -split '\n')) {
        if (-not $line.Trim().StartsWith('|')) { continue }
        $cells = @(Get-Cells $line)
        if ($cells.Count -lt 2) { continue }
        foreach ($match in [regex]::Matches($cells[1], '\bdk_[A-Za-z0-9_]+\b')) { $documented[$match.Value] = $true }
    }
    $registered = @{}; $testPrograms = @{}
    foreach ($tree in @('engine', 'apps', 'tools', 'examples', 'tests')) {
        $directory = Join-Path $repoRoot $tree
        if (-not (Test-Path -LiteralPath $directory)) { continue }
        foreach ($file in (Get-ChildItem -LiteralPath $directory -Recurse -Filter 'CMakeLists.txt' -File)) {
            $cmake = [regex]::Replace((Read-Utf8 $file.FullName), '(?m)#.*$', '')
            foreach ($match in [regex]::Matches($cmake, '(?i)\badd_executable\s*\(\s*(dk_[A-Za-z0-9_]+)\b')) {
                $registered[$match.Groups[1].Value] = $true
                if ($tree -eq 'tests') { $testPrograms[$match.Groups[1].Value] = $true }
            }
        }
    }
    foreach ($target in $documented.Keys) {
        if (-not $registered.ContainsKey($target)) { throw "Unknown test target in selection table: $target" }
    }
    foreach ($target in $testPrograms.Keys) {
        if (-not $documented.ContainsKey($target)) { throw "Test target missing from selection table: $target" }
    }
}

if ($Path.Count) {
    $documents = @($Path | ForEach-Object {
        $candidate = $_
        if (-not [IO.Path]::IsPathRooted($candidate)) { $candidate = Join-Path $repoRoot $candidate }
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
    $body = Read-Utf8 $document.FullName
    $prose = Get-Prose $body
    Test-Tables $prose $document.FullName
    $linkProse = [regex]::Replace($prose, '(`+)[^\n]*?\1', '')
    foreach ($match in [regex]::Matches($linkProse, '\[[^\]]*\]\(([^)]+)\)')) {
        $target = $match.Groups[1].Value
        if ($target -match '^[a-zA-Z][a-zA-Z0-9+.-]*:' -or $target.StartsWith('#')) { continue }
        $target = ($target -split '#')[0]
        if (-not (Test-Path -LiteralPath (Join-Path $document.DirectoryName $target))) { throw "Broken local link: $($document.FullName) -> $target" }
        $linkCount++
    }
    $relative = $document.FullName.Substring($repoRoot.Length + 1).Replace('\', '/')
    if (($relative -match '^spec/(design|development|commands)/' -and $document.Name -ne 'README.md') -or
        ($relative -like 'spec/*' -and $body.StartsWith('---') -and $relative -notlike 'spec/templates/*')) {
        Test-Timestamps (Get-Frontmatter $body $relative) $relative
    }
}
foreach ($file in @('CMakePresets.json', 'vcpkg.json')) { Read-Utf8 (Join-Path $repoRoot $file) | ConvertFrom-Json | Out-Null }
Test-Catalog 'design'
Test-Catalog 'development'
Test-TestEntrypoints
Write-Host "Validated $($documents.Count) Markdown files, $linkCount local links, metadata, tables, indexes, test entrypoints and JSON manifests."
