[CmdletBinding()]
param(
    [string]$CopperExe = '.\build\Release\copper.exe',
    [Parameter(Mandatory = $true)][string]$KiCadCli,
    [Parameter(Mandatory = $true)][string]$SampleRoot,
    [string]$RulesFile = '.\v2_src\jlcpcb_rules.rules',
    [string]$OutputRoot = '.\validation\local_pc',
    [switch]$SkipCorpus
)
$ErrorActionPreference = 'Stop'
function Resolve-File([string]$p, [string]$label) {
    if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { throw "$label not found: $p" }
    return (Resolve-Path -LiteralPath $p).Path
}
$copper = Resolve-File $CopperExe 'Copper executable'
$kicad = Resolve-File $KiCadCli 'KiCad CLI executable'
$rules = Resolve-File $RulesFile 'Fabrication rules'
$sample = (Resolve-Path -LiteralPath $SampleRoot).Path
$run = Join-Path ([System.IO.Path]::GetFullPath($OutputRoot)) (Get-Date -Format 'yyyyMMdd_HHmmss')
New-Item -ItemType Directory -Force -Path $run | Out-Null

$checks = New-Object System.Collections.ArrayList
function Run-Check([string]$name, [string]$exe, [string[]]$args, [string]$log) {
    $out = @(& $exe @args 2>&1); $code = $LASTEXITCODE
    [System.IO.File]::WriteAllText($log, (($out | ForEach-Object { [string]$_ }) -join [Environment]::NewLine), ([System.Text.UTF8Encoding]::new($false)))
    $item = [pscustomobject]@{ name=$name; exit_code=[int]$code; log=$log; output=(($out | ForEach-Object { [string]$_ }) -join [Environment]::NewLine) }
    [void]$checks.Add($item)
    Write-Host ("{0}: {1}" -f $name,$code)
    return $item
}

Run-Check 'copper-help' $copper @('--help') (Join-Path $run 'copper_help.log') | Out-Null
Run-Check 'kicad-version' $kicad @('version') (Join-Path $run 'kicad_version.log') | Out-Null
Run-Check 'rules-validate' $copper @('dev','rules','validate',$rules) (Join-Path $run 'rules_validate.log') | Out-Null

$fixture = Join-Path $PSScriptRoot '..\fixtures\sample_resistor_lol'
$fixture = (Resolve-Path -LiteralPath $fixture).Path
$sch = Join-Path $fixture 'sample_resistor_lol.kicad_sch'
$net = Join-Path $run 'sample_resistor_lol.kicad_net'
Run-Check 'fixture-netlist' $kicad @('sch','export','netlist','--format','kicadsexpr','--output',$net,$sch) (Join-Path $run 'fixture_netlist.log') | Out-Null
if (Test-Path -LiteralPath $net -PathType Leaf) {
    Run-Check 'fixture-topology' $copper @('user','topology','show',$sch,'--netlist',$net,'--out',(Join-Path $run 'fixture_topology.json')) (Join-Path $run 'fixture_topology.log') | Out-Null
}

$board = Join-Path $fixture 'sample_resistor_lol.kicad_pcb'
Run-Check 'fixture-board-inspect' $copper @('user','board-inspect',$board) (Join-Path $run 'fixture_board_inspect.log') | Out-Null
Run-Check 'fixture-board-validate' $copper @('dev','board','validate',$board) (Join-Path $run 'fixture_board_validate.log') | Out-Null
Run-Check 'fixture-dfm' $copper @('user','dfm-check',$board,'--rules',$rules,'--copper-oz','1','--edge','routed','--report',(Join-Path $run 'fixture_dfm.txt')) (Join-Path $run 'fixture_dfm.log') | Out-Null

if (-not $SkipCorpus) {
    & (Join-Path $PSScriptRoot '..\run_kicad_sample_corpus.ps1') `
        -SampleRoot $sample -CopperExe $copper -KiCadCli $kicad -RulesFile $rules `
        -OutputRoot (Join-Path $run 'kicad_corpus')
}

$summary = [pscustomobject]@{ run_directory=$run; copper_exe=$copper; kicad_cli=$kicad; sample_root=$sample; checks=[object[]]$checks.ToArray() }
$summaryPath = Join-Path $run 'local_validation_summary.json'
[System.IO.File]::WriteAllText($summaryPath,(ConvertTo-Json -InputObject $summary -Depth 10),([System.Text.UTF8Encoding]::new($false)))
Write-Host "`nValidation artifacts: $run" -ForegroundColor Green
Write-Host "Summary: $summaryPath"
