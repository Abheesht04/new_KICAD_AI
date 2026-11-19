[CmdletBinding()]
param(
    [int]$Port = 8780,   # 8765 is the Copper Workbench (copper-core serve-http)
    [string]$Python = (Join-Path $env:LOCALAPPDATA 'Programs\Python\Python312\python.exe'),
    [string]$CopperExe = '',
    [string]$KiCadCli = 'C:\Program Files\KiCad\9.0\bin\kicad-cli.exe'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (Test-Path -LiteralPath $Python -PathType Leaf) {
    $pythonExe = (Resolve-Path -LiteralPath $Python).Path
} else {
    $pythonCmd = Get-Command $Python -ErrorAction Stop
    if ($pythonCmd.CommandType -eq 'Alias') {
        $resolved = Get-Command $pythonCmd.Definition -ErrorAction Stop
        $pythonExe = $resolved.Source
    } else {
        $pythonExe = $pythonCmd.Source
    }
}
if (-not $pythonExe -or -not (Test-Path -LiteralPath $pythonExe -PathType Leaf)) {
    throw "Python executable could not be resolved: $Python"
}
Write-Host ('Python: ' + $pythonExe) -ForegroundColor DarkGray
if (-not (Test-Path -LiteralPath (Join-Path $root 'copper_lab\server.py'))) {
    throw 'copper_lab\server.py was not found. Run this script from the Copper project bundle.'
}
if (-not $CopperExe) {   # bundle layout (build\Release) or this repository's single-config build (build)
    $CopperExe = @('.\build\Release\copper.exe', '.\build\copper.exe') | Where-Object { Test-Path -LiteralPath (Join-Path $root $_) } | Select-Object -First 1
    if (-not $CopperExe) { $CopperExe = '.\build\Release\copper.exe' }
}
$exePath = if ([System.IO.Path]::IsPathRooted($CopperExe)) { $CopperExe } else { Join-Path $root $CopperExe }
$kicadPath = if ([System.IO.Path]::IsPathRooted($KiCadCli)) { $KiCadCli } else { Join-Path $root $KiCadCli }
if (Test-Path -LiteralPath $exePath) { $env:COPPER_EXE = (Resolve-Path -LiteralPath $exePath).Path }
if (Test-Path -LiteralPath $kicadPath) { $env:KICAD_CLI = (Resolve-Path -LiteralPath $kicadPath).Path }

Write-Host ''
Write-Host '  COPPER LAB' -ForegroundColor Green
Write-Host '  Local engineering workspace' -ForegroundColor DarkGray
Write-Host ''
Write-Host ('  Copper: ' + $(if ($env:COPPER_EXE) { $env:COPPER_EXE } else { 'not found — set COPPER_EXE' }))
Write-Host ('  KiCad:  ' + $(if ($env:KICAD_CLI) { $env:KICAD_CLI } else { 'not found — set KICAD_CLI' }))
Write-Host '  Designs: copper.design-intent JSON written by Manus (workspace D:\copper-manus, see manus\README.md)'
Write-Host ''
Write-Host "  Open http://127.0.0.1:$Port in your browser" -ForegroundColor Cyan
Write-Host '  Keep this PowerShell window open. Press Ctrl+C to stop.' -ForegroundColor DarkGray
Write-Host ''
& $pythonExe (Join-Path $root 'copper_lab\server.py') --host 127.0.0.1 --port $Port
