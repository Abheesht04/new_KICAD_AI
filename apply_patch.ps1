$ErrorActionPreference = 'Stop'
$root = (Get-Location).Path
$files = @(
    @{ Source = Join-Path $PSScriptRoot 'v2_src\design_parity.c'; Destination = Join-Path $root 'v2_src\design_parity.c' },
    @{ Source = Join-Path $PSScriptRoot 'v2_src\tests\test_design.c'; Destination = Join-Path $root 'v2_src\tests\test_design.c' }
)
foreach ($f in $files) {
    if (-not (Test-Path -LiteralPath $f.Destination)) { throw "Expected Copper source file not found: $($f.Destination)" }
    Copy-Item -LiteralPath $f.Source -Destination $f.Destination -Force
    Write-Host "Updated $($f.Destination)" -ForegroundColor Green
}
Write-Host 'Patch applied. Rebuild Copper Release and run CTest.' -ForegroundColor Cyan
