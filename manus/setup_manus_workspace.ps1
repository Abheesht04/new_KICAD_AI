<#
Create the folder a Manus agent works in (Manus Studio -> "Work locally" -> "Choose a folder").

    .\manus\setup_manus_workspace.ps1 [-Workspace D:\copper-manus] [-KiCadCli <path>] [-KiCadShare <dir with symbols\ and footprints\>]
                                      [-Python <python.exe>] [-Force]

It copies MANUS.md, copper.ps1 and the examples into the workspace and writes copper.config.json with absolute paths
to this repository's build, the Python design compiler and KiCad. KiCad 9.x is preferred (C:\Program Files\KiCad\9.0);
anything else is recorded and reported by `copper.ps1 doctor` as a development check, not native evidence.
Nothing is installed and nothing outside the workspace is changed.
#>
[CmdletBinding()]
param(
    [string]$Workspace = 'D:\copper-manus',
    [string]$KiCadCli = '',
    [string]$KiCadShare = '',
    [string]$Python = '',
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$template = Join-Path $repo 'manus\workspace'

function First([string[]]$candidates) { foreach ($c in $candidates) { if ($c -and (Test-Path -LiteralPath $c)) { return (Resolve-Path -LiteralPath $c).Path } }; return '' }

$copper = First @((Join-Path $repo 'build\Release\copper.exe'), (Join-Path $repo 'build\copper.exe'))
$core = First @((Join-Path $repo 'build\Release\copper-core.exe'), (Join-Path $repo 'build\copper-core.exe'))
if (-not $Python) {
    $Python = First @((Join-Path $env:LOCALAPPDATA 'Programs\Python\Python312\python.exe'), (Join-Path $env:LOCALAPPDATA 'Programs\Python\Python311\python.exe'), (Join-Path $env:LOCALAPPDATA 'Programs\Python\Python310\python.exe'))
}
if (-not $KiCadCli) { $KiCadCli = First @('C:\Program Files\KiCad\9.0\bin\kicad-cli.exe', $env:KICAD_CLI) }
if (-not $KiCadShare) {
    $guess = if ($KiCadCli) { Join-Path (Split-Path -Parent (Split-Path -Parent $KiCadCli)) 'share\kicad' } else { '' }
    $KiCadShare = First @('C:\Program Files\KiCad\9.0\share\kicad', $guess)
}
$cfg = [ordered]@{
    repo            = $repo
    copper          = $copper
    copper_core     = $core
    python          = $Python
    kicad_cli       = $KiCadCli
    symbols_root    = if ($KiCadShare) { Join-Path $KiCadShare 'symbols' } else { '' }
    footprints_root = if ($KiCadShare) { Join-Path $KiCadShare 'footprints' } else { '' }
    compiler        = Join-Path $repo 'src\v2_src\tools\copper_design_compile.py'
    asset_context   = Join-Path $repo 'src\v2_src\tools\kicad_asset_context.py'
    intent_schema   = Join-Path $repo 'src\v2_src\schemas\copper.design-intent.v1.schema.json'
    fab_profile     = Join-Path $repo 'fab_profiles\jlcpcb_2layer.json'
    templates       = Join-Path $repo 'design_templates\copper_templates.v1.json'
    routing_rules   = Join-Path $repo 'routing_profiles\copper_2layer_default.json'
}

if ((Test-Path -LiteralPath $Workspace) -and -not $Force) {
    if (Get-ChildItem -LiteralPath $Workspace -Force | Select-Object -First 1) { throw "$Workspace exists and is not empty; use -Force to refresh MANUS.md, copper.ps1, examples and the config (designs\ is kept)" }
}
New-Item -ItemType Directory -Force -Path $Workspace, (Join-Path $Workspace 'designs'), (Join-Path $Workspace 'examples') | Out-Null
Copy-Item -Force -LiteralPath (Join-Path $template 'MANUS.md'), (Join-Path $template 'copper.ps1'), (Join-Path $template 'copper-prompt.ps1'), (Join-Path $template 'manus_requirements.schema.json') -Destination $Workspace
Remove-Item -LiteralPath (Join-Path $Workspace 'manus_output.schema.json') -ErrorAction SilentlyContinue   # Manus no longer writes designs
Copy-Item -Force -Path (Join-Path $template 'examples\*') -Destination (Join-Path $Workspace 'examples')
Copy-Item -Force -LiteralPath $cfg.intent_schema -Destination (Join-Path $Workspace 'copper.design-intent.v1.schema.json')
[IO.File]::WriteAllText((Join-Path $Workspace 'copper.config.json'), ($cfg | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))
# The key file copper-prompt.ps1 reads. Never overwritten; readable by this Windows user only.
$keyFile = Join-Path $Workspace 'manus_api_key.txt'
if (-not (Test-Path -LiteralPath $keyFile)) {
    [IO.File]::WriteAllText($keyFile, "# Paste your Manus API key on the line below (replace the placeholder), then save.`r`n# Keep this file private: do not copy it into the repository or share it.`r`nPASTE-YOUR-MANUS-API-KEY-HERE`r`n", (New-Object Text.UTF8Encoding($false)))
    & icacls $keyFile /inheritance:r /grant:r "$($env:USERNAME):(R,W)" | Out-Null
}

Write-Host "Manus workspace ready: $Workspace" -ForegroundColor Green
$cfg.GetEnumerator() | ForEach-Object { '{0,-16} {1}{2}' -f $_.Key, $_.Value, $(if ($_.Value -and (Test-Path -LiteralPath $_.Value)) { '' } else { '   <-- MISSING' }) }
Write-Host ''
Write-Host 'Next: in Manus Studio, turn on "Work locally", choose this folder, and start the task with:' -ForegroundColor Cyan
Write-Host '  "Read MANUS.md in this folder first and follow it."'
