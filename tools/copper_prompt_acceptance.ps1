# Prompt acceptance on Windows: a thin wrapper around tools/prompt_acceptance.py.
# All logic, encoding and classification live in the Python harness, so PowerShell's file and console
# encodings cannot alter the prompts (Windows PowerShell 5.1 reads files as ANSI unless told otherwise).
#
#   .\tools\copper_prompt_acceptance.ps1 -Pcba .\build\pcba.exe [-VerifyNative -KicadCli PATH] [-RunEsp32Design]
param(
    [Parameter(Mandatory = $true)][string]$Pcba,
    [string]$Out,
    [switch]$VerifyNative,
    [string]$KicadCli,
    [switch]$RunEsp32Design,
    [string]$Symbols,
    [string]$Footprints
)
$env:PYTHONUTF8 = '1'
$python = if ($env:PCBA_PYTHON) { $env:PCBA_PYTHON } else { 'python' }
$argList = @((Join-Path $PSScriptRoot 'prompt_acceptance.py'), '--pcba', $Pcba)
if ($Out) { $argList += @('--out', $Out) }
if ($VerifyNative) { $argList += '--native' }
if ($KicadCli) { $argList += @('--kicad-cli', $KicadCli) }
if ($RunEsp32Design) { $argList += '--esp32' }
if ($Symbols) { $argList += @('--symbols', $Symbols) }
if ($Footprints) { $argList += @('--footprints', $Footprints) }
& $python @argList
exit $LASTEXITCODE
