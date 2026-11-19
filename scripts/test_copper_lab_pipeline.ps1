[CmdletBinding()]
param(
    [string]$Python = (Join-Path $env:LOCALAPPDATA 'Programs\Python\Python312\python.exe'),
    [string]$CopperExe = '.\build\Release\copper.exe'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path -LiteralPath $Python -PathType Leaf)) {
    $cmd = Get-Command $Python -ErrorAction Stop
    if ($cmd.CommandType -eq 'Alias') { $cmd = Get-Command $cmd.Definition -ErrorAction Stop }
    $Python = $cmd.Source
}
if (-not (Test-Path -LiteralPath $Python -PathType Leaf)) { throw "Python executable not found: $Python" }
$exe = if ([IO.Path]::IsPathRooted($CopperExe)) { $CopperExe } else { Join-Path $root $CopperExe }
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw "Copper Release executable not found: $exe" }
$env:COPPER_CLI = (Resolve-Path -LiteralPath $exe).Path
Push-Location $root
try {
    Write-Host "Python: $Python" -ForegroundColor Cyan
    Write-Host "Copper: $env:COPPER_CLI" -ForegroundColor Cyan
    & $Python -m py_compile .\copper_lab\server.py .\copper_lab\design_pipeline.py .\v2_src\tools\copper_design_compile.py .\v2_src\tools\kicad_asset_context.py
    if ($LASTEXITCODE -ne 0) { throw 'Python syntax check failed.' }
    & $Python -m unittest discover -s .\copper_lab\tests -v
    if ($LASTEXITCODE -ne 0) { throw 'Copper Lab web integration tests failed.' }
    & $Python -m unittest discover -s .\v2_src\tests -p test_design_compile.py -v
    if ($LASTEXITCODE -ne 0) { throw 'Copper design compiler tests failed.' }
    Write-Host 'ALL PYTHON / COPPER CLI INTEGRATION TESTS PASSED' -ForegroundColor Green
}
finally { Pop-Location }
