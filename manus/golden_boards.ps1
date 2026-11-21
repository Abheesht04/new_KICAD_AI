<#
T5 of the testing loop (docs/engineering/PIPELINE_HARD_RULES.md): regenerate the golden boards twice from
manus/golden/<board>/{prompt.txt,requirements.json} and compare them with manus/golden/baseline.json.

    .\manus\golden_boards.ps1 -Workspace <configured Copper workspace> -Out <new folder> [-Baseline <file>]

The workspace is one made by setup_manus_workspace.ps1 or selftest.ps1 (it holds copper.ps1 and copper.config.json).
Nothing calls Manus; no credits are spent.

Per board:
  FAIL       routing not ROUTED, DRC unconnected items, a higher ERC/DRC count, a DFM verdict change, or two runs
             that are not byte-identical (H5);
  CHANGED    the numbers are no worse but a file hash differs from the baseline: allowed only with a stated
             reason and the owner's approval (H9);
  PASS       identical to the baseline.
Exit 0 when every board is PASS, 3 when the worst is CHANGED, 1 on any FAIL, 2 on bad arguments.
#>
param([Parameter(Mandatory)][string]$Workspace, [Parameter(Mandatory)][string]$Out, [string]$Baseline = '')
$ErrorActionPreference = 'Stop'
$golden = Join-Path $PSScriptRoot 'golden'
if (-not $Baseline) { $Baseline = Join-Path $golden 'baseline.json' }   # another file only to test this script
$base = Get-Content $Baseline -Raw | ConvertFrom-Json
$copper = Join-Path $Workspace 'copper.ps1'
if (-not (Test-Path $copper)) { Write-Host "no copper.ps1 in $Workspace"; exit 2 }
if (Test-Path $Out) { Write-Host "$Out already exists; give a new folder"; exit 2 }
New-Item -ItemType Directory $Out | Out-Null

function Invoke-Copper([string[]]$argv) {
    $o = & powershell -NoProfile -ExecutionPolicy Bypass -File $copper @argv 2>&1 | Out-String
    $line = ($o -split "`n" | Where-Object { $_ -like 'COPPER_RESULT *' } | Select-Object -Last 1)
    if (-not $line) { throw "no COPPER_RESULT from copper.ps1 $($argv -join ' ')`n$o" }
    return ($line.Substring(14) | ConvertFrom-Json)
}
function Get-Sev($block, [string]$sev) { if ($block -and $block.by_severity -and $block.by_severity.$sev) { [int]$block.by_severity.$sev } else { 0 } }

$rows = @()
$worst = 0   # 0 PASS, 3 CHANGED, 1 FAIL
foreach ($prop in $base.boards.PSObject.Properties) {
    $name = $prop.Name; $b = $prop.Value
    $runs = @()
    foreach ($run in 1, 2) {
        $d = Join-Path $Out "${name}_run$run"
        New-Item -ItemType Directory $d | Out-Null
        Copy-Item (Join-Path $golden "$name\prompt.txt"), (Join-Path $golden "$name\requirements.json") $d
        $null = Invoke-Copper @('generate', '-Requirements', "$d\requirements.json", '-PromptFile', "$d\prompt.txt", '-Out', $d)
        $null = Invoke-Copper @('compile', '-Intent', "$d\intent.json", '-Out', $d)
        $p = Join-Path $d $b.project
        $null = Invoke-Copper @('report', '-Project', $p)
        $f = Get-Content (Join-Path $p 'copper_facts.json') -Raw | ConvertFrom-Json
        $hash = { param($x) if (Test-Path $x) { (Get-FileHash $x).Hash } else { 'MISSING' } }
        $runs += [pscustomobject]@{
            routing = $f.routing; drc_unconnected = [int]$f.drc_unconnected
            erc_error = Get-Sev $f.erc 'error'; erc_warning = Get-Sev $f.erc 'warning'
            drc_error = Get-Sev $f.drc 'error'; drc_warning = Get-Sev $f.drc 'warning'
            dfm = $f.dfm_verdict; kicad = $f.drc.kicad_version
            all = (@("$d\intent.json", "$d\routing.json", "$p\$($b.project).kicad_sch", "$p\$($b.project).kicad_pcb") | ForEach-Object { & $hash $_ }) -join ','
            pcb_sha256 = & $hash "$p\$($b.project).kicad_pcb"; sch_sha256 = & $hash "$p\$($b.project).kicad_sch"
        }
    }
    $r = $runs[0]
    $why = @()
    if ($r.routing -ne 'ROUTED') { $why += "routing $($r.routing)" }
    if ($r.drc_unconnected -gt 0) { $why += "$($r.drc_unconnected) unconnected" }
    foreach ($k in 'erc_error', 'erc_warning', 'drc_error', 'drc_warning') { if ($r.$k -gt $b.$k) { $why += "$k $($r.$k) > $($b.$k)" } }
    if ($r.dfm -ne $b.dfm) { $why += "dfm $($r.dfm) (baseline $($b.dfm))" }
    if ($runs[0].all -ne $runs[1].all) { $why += 'two runs differ (H5)' }
    $verdict = 'PASS'
    if ($why.Count) { $verdict = 'FAIL'; $worst = 1 }
    elseif ($r.pcb_sha256 -ne $b.pcb_sha256 -or $r.sch_sha256 -ne $b.sch_sha256) {
        $verdict = 'CHANGED'; $why += 'file hash differs from the baseline: state the reason (H9)'
        if ($worst -eq 0) { $worst = 3 }
    }
    $rows += [pscustomobject]@{
        board = $name; verdict = $verdict; routing = $r.routing; unconnected = $r.drc_unconnected
        erc = "$($r.erc_error)/$($r.erc_warning)"; drc = "$($r.drc_error)/$($r.drc_warning)"; dfm = $r.dfm; kicad = $r.kicad
        deterministic = ($runs[0].all -eq $runs[1].all); detail = ($why -join '; ')
        pcb_sha256 = $r.pcb_sha256; sch_sha256 = $r.sch_sha256
    }
}
$rows | ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $Out 'golden_boards.json')
$rows | Format-Table board, verdict, routing, unconnected, erc, drc, dfm, deterministic, detail -AutoSize -Wrap | Out-Host
$label = @{ 0 = 'PASS'; 3 = 'CHANGED'; 1 = 'FAIL' }[$worst]
Write-Host "GOLDEN_BOARDS $label  ($Out\golden_boards.json)"
exit $worst
