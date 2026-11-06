[CmdletBinding()]
param(
    [string]$BoardPath = '',
    [string]$KicadCli = 'C:\Program Files\KiCad\9.0\bin\kicad-cli.exe',
    [string]$OutputRoot = '.\out\cam-kicad906-sample'
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $KicadCli -PathType Leaf)) {
    throw "KiCad CLI not found: $KicadCli"
}

if ([string]::IsNullOrWhiteSpace($BoardPath)) {
    $searchRoot = Join-Path (Get-Location).Path 'out\stress\divider'
    if (-not (Test-Path -LiteralPath $searchRoot -PathType Container)) {
        throw "BoardPath was not supplied and default search folder does not exist: $searchRoot"
    }

    $boards = @(Get-ChildItem -LiteralPath $searchRoot -Recurse -File -Filter '*.kicad_pcb')
    if ($boards.Count -eq 0) {
        throw "No .kicad_pcb file found under $searchRoot. Supply -BoardPath explicitly."
    }
    if ($boards.Count -gt 1) {
        $boards | Select-Object -ExpandProperty FullName | ForEach-Object { Write-Host $_ }
        throw 'More than one board was found. Re-run with -BoardPath set to the exact board file.'
    }
    $BoardPath = $boards[0].FullName
}

$BoardPath = (Resolve-Path -LiteralPath $BoardPath).Path
if (-not $BoardPath.EndsWith('.kicad_pcb', [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Expected a .kicad_pcb input, received: $BoardPath"
}

if ([System.IO.Path]::IsPathRooted($OutputRoot)) {
    $root = $OutputRoot
} else {
    $root = Join-Path (Get-Location).Path $OutputRoot
}

$gerberDir = Join-Path $root 'gerbers-x2'
$drillAlternateDir = Join-Path $root 'drill-alternate'
$drillRouteDir = Join-Path $root 'drill-route'
New-Item -ItemType Directory -Force -Path $gerberDir, $drillAlternateDir, $drillRouteDir | Out-Null

function Invoke-KiCadChecked {
    param([string[]]$Arguments, [string]$Description)
    Write-Host "`n== $Description ==" -ForegroundColor Cyan
    & $script:KicadCli @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with KiCad exit code $LASTEXITCODE"
    }
}

Write-Host "KiCad CLI: $KicadCli"
Write-Host "Board:     $BoardPath"
Write-Host "Output:    $root"

& $KicadCli version 2>&1 | Set-Content -LiteralPath (Join-Path $root 'kicad-version.txt') -Encoding UTF8
if ($LASTEXITCODE -ne 0) { throw "kicad-cli version failed with exit code $LASTEXITCODE" }

& $KicadCli pcb export gerbers --help 2>&1 |
    Set-Content -LiteralPath (Join-Path $root 'gerbers-help.txt') -Encoding UTF8
if ($LASTEXITCODE -ne 0) { throw "Gerber help failed with exit code $LASTEXITCODE" }

& $KicadCli pcb export drill --help 2>&1 |
    Set-Content -LiteralPath (Join-Path $root 'drill-help.txt') -Encoding UTF8
if ($LASTEXITCODE -ne 0) { throw "Drill help failed with exit code $LASTEXITCODE" }

# X2 and aperture macros are enabled by default. Precision 6 gives the
# KiCad metric 4.6 coordinate form targeted by the first Gerber parser pass.
Invoke-KiCadChecked -Description 'Gerber X2 export (all board layers)' -Arguments @(
    'pcb', 'export', 'gerbers',
    '--output', $gerberDir,
    '--precision', '6',
    $BoardPath
)

# Decimal coordinates and absolute origin avoid guessing the coordinate
# format during the initial Excellon reader milestone. PTH and NPTH are split.
Invoke-KiCadChecked -Description 'Excellon drill export (alternate oval-slot format)' -Arguments @(
    'pcb', 'export', 'drill',
    '--output', $drillAlternateDir,
    '--format', 'excellon',
    '--drill-origin', 'absolute',
    '--excellon-zeros-format', 'decimal',
    '--excellon-units', 'mm',
    '--excellon-oval-format', 'alternate',
    '--excellon-separate-th',
    '--generate-map',
    '--map-format', 'gerberx2',
    $BoardPath
)

# Second drill export exercises routed oval slots (G00/G01 style where emitted).
Invoke-KiCadChecked -Description 'Excellon drill export (routed oval-slot format)' -Arguments @(
    'pcb', 'export', 'drill',
    '--output', $drillRouteDir,
    '--format', 'excellon',
    '--drill-origin', 'absolute',
    '--excellon-zeros-format', 'decimal',
    '--excellon-units', 'mm',
    '--excellon-oval-format', 'route',
    '--excellon-separate-th',
    $BoardPath
)

$manifest = [ordered]@{
    board = $BoardPath
    kicad_cli = $KicadCli
    export_profile = 'KiCad 9.x; Gerber X2; precision 6; Excellon decimal mm; absolute origin; PTH/NPTH separate'
    directories = @('gerbers-x2', 'drill-alternate', 'drill-route')
}
$manifest | ConvertTo-Json -Depth 4 |
    Set-Content -LiteralPath (Join-Path $root 'cam-export-manifest.json') -Encoding UTF8

$zipPath = "$root.zip"
if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
Compress-Archive -Path (Join-Path $root '*') -DestinationPath $zipPath -Force

Write-Host "`nExport complete." -ForegroundColor Green
Write-Host "CAM folder: $root"
Write-Host "ZIP:        $zipPath"
Write-Host "Review the two help files and upload the ZIP."
