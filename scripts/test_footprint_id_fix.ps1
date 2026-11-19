[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$PythonExe = Join-Path $env:LOCALAPPDATA 'Programs\Python\Python312\python.exe'
if (-not (Test-Path -LiteralPath $PythonExe)) {
    $pythonCommand = Get-Command python -ErrorAction Stop
    $PythonExe = $pythonCommand.Source
}
if (-not (Test-Path -LiteralPath $PythonExe)) {
    throw "Python executable not found: $PythonExe"
}
$CopperExe = Join-Path $ProjectRoot 'build\Release\copper.exe'
$OldCopperCli = $env:COPPER_CLI
try {
    if (Test-Path -LiteralPath $CopperExe) {
        $env:COPPER_CLI = $CopperExe
        Write-Host "Copper CLI integration enabled: $CopperExe" -ForegroundColor Cyan
    } else {
        Remove-Item Env:COPPER_CLI -ErrorAction SilentlyContinue
        Write-Host 'Copper CLI integration skipped: Release executable not found.' -ForegroundColor Yellow
    }
    Write-Host "Python: $PythonExe" -ForegroundColor Cyan
    Push-Location $ProjectRoot
    try {
        & $PythonExe -m unittest discover -s 'v2_src\tests' -p 'test_design_compile.py' -v
        if ($LASTEXITCODE -ne 0) { throw "Design compiler tests failed with exit code $LASTEXITCODE" }
    } finally {
        Pop-Location
    }
    Write-Host 'Footprint ID normalization tests passed.' -ForegroundColor Green
} finally {
    if ($null -eq $OldCopperCli) {
        Remove-Item Env:COPPER_CLI -ErrorAction SilentlyContinue
    } else {
        $env:COPPER_CLI = $OldCopperCli
    }
}
