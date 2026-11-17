[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SampleRoot,
    [string]$CopperExe = (Join-Path $PSScriptRoot '..\..\build\Release\copper.exe'),
    [string]$KiCadCli = 'kicad-cli',
    [string]$RulesFile = (Join-Path $PSScriptRoot '..\jlcpcb_rules.rules'),
    [string]$OutputRoot = (Join-Path $PSScriptRoot '..\..\kicad_sample_validation'),
    [switch]$SkipBoardCorpus
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $SampleRoot).Path
$copper = (Resolve-Path -LiteralPath $CopperExe).Path
if (Test-Path -LiteralPath $KiCadCli -PathType Leaf) {
    $kicad = (Resolve-Path -LiteralPath $KiCadCli -ErrorAction Stop).Path
} else {
    $cliCommand = Get-Command $KiCadCli -ErrorAction Stop
    $kicad = if ($cliCommand.Source) { $cliCommand.Source } else { $cliCommand.Path }
}
$run = Join-Path ([System.IO.Path]::GetFullPath($OutputRoot)) (Get-Date -Format 'yyyyMMdd_HHmmss')
$topologyRoot = Join-Path $run 'topology'
New-Item -ItemType Directory -Force -Path $topologyRoot | Out-Null

function Invoke-Logged {
    param([string]$Exe, [string[]]$Args, [string]$Log)
    $output = @(& $Exe @Args 2>&1)
    $code = $LASTEXITCODE
    [System.IO.File]::WriteAllText($Log, (($output | ForEach-Object { [string]$_ }) -join [Environment]::NewLine),
        ([System.Text.UTF8Encoding]::new($false)))
    return [pscustomobject]@{ ExitCode = [int]$code; Output = (($output | ForEach-Object { [string]$_ }) -join [Environment]::NewLine) }
}

$schematics = @(Get-ChildItem -LiteralPath $root -Filter '*.kicad_sch' -File -Recurse | Sort-Object FullName)
$boards = @(Get-ChildItem -LiteralPath $root -Filter '*.kicad_pcb' -File -Recurse | Sort-Object FullName)
if ($schematics.Count -eq 0 -and $boards.Count -eq 0) {
    throw "No KiCad schematic or PCB files found under $root"
}

$results = New-Object System.Collections.ArrayList
foreach ($sch in $schematics) {
    $relative = $sch.FullName.Substring($root.Length).TrimStart('\', '/')
    $safe = [System.IO.Path]::GetFileNameWithoutExtension($sch.Name) -replace '[^A-Za-z0-9._-]', '_'
    $caseDir = Join-Path $topologyRoot $safe
    New-Item -ItemType Directory -Force -Path $caseDir | Out-Null
    $netlist = Join-Path $caseDir 'fresh_netlist.kicad_net'
    $netLog = Join-Path $caseDir 'kicad_netlist.log'
    $topologyJson = Join-Path $caseDir 'topology.json'
    $net = Invoke-Logged -Exe $kicad -Args @('sch','export','netlist','--format','kicadsexpr','--output',$netlist,$sch.FullName) -Log $netLog
    $top = $null
    if ($net.ExitCode -eq 0 -and (Test-Path -LiteralPath $netlist -PathType Leaf)) {
        $top = Invoke-Logged -Exe $copper -Args @('user','topology','show',$sch.FullName,'--netlist',$netlist,'--out',$topologyJson) -Log (Join-Path $caseDir 'copper_topology.log')
    }
    $item = [pscustomobject]@{
        Schematic = $relative
        NetlistExitCode = $net.ExitCode
        TopologyExitCode = if ($top) { $top.ExitCode } else { $null }
        Netlist = if (Test-Path -LiteralPath $netlist) { $netlist } else { $null }
        TopologyJson = if (Test-Path -LiteralPath $topologyJson) { $topologyJson } else { $null }
        OutputDirectory = $caseDir
    }
    [void]$results.Add($item)
    Write-Host ("SCHEMATIC {0}: netlist={1} topology={2}" -f $relative,$net.ExitCode,$item.TopologyExitCode)
}

$boardSummary = $null
if (-not $SkipBoardCorpus -and $boards.Count -gt 0) {
    if (-not (Test-Path -LiteralPath $RulesFile -PathType Leaf)) {
        Write-Warning "Rules file not found; skipping DFM/DRC board corpus. Supply -RulesFile to enable it."
    } else {
        $boardOutput = Join-Path $run 'boards'
        & (Join-Path $PSScriptRoot 'run_board_corpus.ps1') -BoardRoot $root `
            -CopperExe $copper -RulesFile (Resolve-Path $RulesFile).Path `
            -OutputRoot $boardOutput -KiCadCli $kicad
        $boardSummary = $boardOutput
    }
}

$summary = [pscustomobject]@{
    SampleRoot = $root
    SchematicCount = $schematics.Count
    BoardCount = $boards.Count
    TopologyResults = [object[]]$results.ToArray()
    BoardCorpusOutput = $boardSummary
}
$summaryPath = Join-Path $run 'kicad_sample_corpus_summary.json'
$json = ConvertTo-Json -InputObject $summary -Depth 10
[System.IO.File]::WriteAllText($summaryPath,$json,([System.Text.UTF8Encoding]::new($false)))
Write-Host "`nKiCad sample corpus complete."
Write-Host "Schematics: $($schematics.Count); boards: $($boards.Count)"
Write-Host "Summary: $summaryPath"
