<#
Self-test of the Manus setup: does what MANUS.md tells the agent to do, in a throwaway workspace, and checks each
COPPER_RESULT. It does not need Manus itself (the design intent is the committed LED example).

    .\manus\selftest.ps1 [-KiCadCli <path>] [-KiCadShare <dir>]

Prints PASS / FAIL / BLOCKED per step and exits 0 only when every step that could run passed.
#>
[CmdletBinding()]
param([string]$KiCadCli = '', [string]$KiCadShare = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$ws = Join-Path $env:TEMP ('copper-manus-selftest-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
$results = New-Object System.Collections.Generic.List[object]
function Step([string]$name, [string]$status, [string]$detail) {
    $results.Add([pscustomobject]@{ step = $name; status = $status; detail = $detail })
    $color = switch ($status) { 'PASS' { 'Green' } 'BLOCKED' { 'Yellow' } default { 'Red' } }
    Write-Host ('[{0}] {1}  {2}' -f $status, $name, $detail) -ForegroundColor $color
}
function Copper([string[]]$argv) {
    $ErrorActionPreference = 'Continue'   # a tool's stderr is evidence, not a script error (PowerShell 5.1)
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $ws 'copper.ps1') @argv 2>&1 | ForEach-Object { "$_" }
    $line = $out | Where-Object { $_ -like 'COPPER_RESULT *' } | Select-Object -Last 1
    $r = if ($line) { $line.Substring(14) | ConvertFrom-Json } else { $null }
    return [pscustomobject]@{ exit = $LASTEXITCODE; result = $r; text = ($out -join "`n") }
}

$setupArgs = @('-Workspace', $ws)
if ($KiCadCli) { $setupArgs += @('-KiCadCli', $KiCadCli) }
if ($KiCadShare) { $setupArgs += @('-KiCadShare', $KiCadShare) }
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo 'manus\setup_manus_workspace.ps1') @setupArgs | Out-Null
if ($LASTEXITCODE -ne 0 -or -not (Test-Path (Join-Path $ws 'copper.config.json'))) { Step 'setup' 'FAIL' 'workspace not created'; exit 1 }
Step 'setup' 'PASS' $ws

$d = Copper @('doctor')
if ($d.result -and $d.result.outcome -eq 'READY') { Step 'doctor' 'PASS' ("kicad " + $d.result.kicad_version + '; ' + $d.result.note) }
else { Step 'doctor' 'BLOCKED' ('missing: ' + (($d.result.missing) -join ', ')) }
$ready = $d.result -and $d.result.outcome -eq 'READY'

if ($ready) {
    $ctx = Join-Path $ws 'designs\led\context.json'
    $c = Copper @('context', '-Prompt', 'symbol Device:R footprint Resistor_SMD:R_0603_1608Metric symbol Device:LED footprint LED_SMD:LED_0603_1608Metric symbol Connector_Generic:Conn_01x02 footprint Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical', '-Out', $ctx)
    $ok = $c.exit -eq 0 -and (Test-Path $ctx)
    if ($ok) {
        $j = Get-Content -Raw $ctx | ConvertFrom-Json
        $led = @($j.symbols) | Where-Object { $_.id -eq 'Device:LED' -or $_.symbol_id -eq 'Device:LED' } | Select-Object -First 1
        Step 'context' 'PASS' ("symbols " + @($j.symbols).Count + ', footprints ' + @($j.footprints).Count + $(if ($led) { '; Device:LED pins ' + (($led.pins | ForEach-Object { "$($_.number)=$($_.name)" }) -join ' ') } else { '' }))
    } else { Step 'context' 'FAIL' $c.text }

    $intent = Join-Path $ws 'designs\led\intent.json'
    Copy-Item (Join-Path $ws 'examples\led_indicator.intent.json') $intent
    $k = Copper @('compile', '-Intent', $intent, '-Out', (Join-Path $ws 'designs\led'))
    $proj = Join-Path $ws 'designs\led\LedIndicator'
    if ($k.exit -eq 0 -and (Test-Path (Join-Path $proj 'LedIndicator.kicad_pcb'))) { Step 'compile' 'PASS' ($k.result.outcome) } else { Step 'compile' 'FAIL' $k.text }

    $bad = Join-Path $ws 'designs\bad\intent.json'
    New-Item -ItemType Directory -Force (Split-Path $bad) | Out-Null
    $bj = Get-Content -Raw $intent | ConvertFrom-Json          # make D1 pin 2 a single-ended net: must be rejected
    ($bj.components | Where-Object { $_.reference -eq 'D1' }).pins[1].net = 'ONLY_ONE'
    [IO.File]::WriteAllText($bad, ($bj | ConvertTo-Json -Depth 8), (New-Object Text.UTF8Encoding($false)))
    $kb = Copper @('compile', '-Intent', $bad, '-Out', (Join-Path $ws 'designs\bad'))
    if ($kb.exit -eq 2 -and $kb.result.outcome -eq 'INVALID_INPUT') { Step 'compile rejects a bad intent' 'PASS' 'exit 2 INVALID_INPUT' } else { Step 'compile rejects a bad intent' 'FAIL' $kb.text }

    $ch = Copper @('checks', '-Project', $proj)
    if ($ch.result -and $ch.result.outcome -in @('FAIL', 'NO_ERRORS_SEE_WARNINGS')) { Step 'checks' 'PASS' ("ERC/DRC read: " + $ch.result.outcome) } else { Step 'checks' 'FAIL' $ch.text }

    $pcb = Join-Path $proj 'LedIndicator.kicad_pcb'
    $in = Copper @('inspect', '-Board', $pcb)
    if ($in.exit -eq 0 -and $in.text -match 'Footprints: 3') { Step 'inspect' 'PASS' '3 footprints' } else { Step 'inspect' 'FAIL' $in.text }

    $f = Copper @('dfm', '-Board', $pcb)
    if ($f.result -and $f.result.outcome -in @('PASS_OR_UNVERIFIED_SEE_JSON', 'FAIL')) { Step 'dfm' 'PASS' ("engine verdict reported, exit " + $f.exit) } else { Step 'dfm' 'FAIL' $f.text }

    $net = Join-Path $proj 'validation\design.kicad_net'
    $t = Copper @('topology', '-Schematic', (Join-Path $proj 'LedIndicator.kicad_sch'), '-Netlist', $net)
    if ($t.exit -eq 0 -and $t.text -match 'Components: 3') { Step 'topology' 'PASS' ($t.text -split "`n" | Where-Object { $_ -match 'Components:' } | Select-Object -First 1).Trim() } else { Step 'topology' 'FAIL' $t.text }
}

# Copper Lab's own test script (Python compiler and web-pipeline tests).
$lab = Join-Path $repo 'scripts\test_copper_lab_pipeline.ps1'
$ErrorActionPreference = 'Continue'
$labOut = & powershell -NoProfile -ExecutionPolicy Bypass -File $lab 2>&1 | ForEach-Object { "$_" }
if ($LASTEXITCODE -eq 0) { Step 'copper lab tests' 'PASS' (($labOut | Select-String -Pattern '^Ran \d+ tests' | ForEach-Object { $_.Line }) -join '; ') }
else { Step 'copper lab tests' 'FAIL' (($labOut | Select-Object -Last 15) -join "`n") }


$failed = @($results | Where-Object { $_.status -eq 'FAIL' }).Count
$blocked = @($results | Where-Object { $_.status -eq 'BLOCKED' }).Count
Write-Host ''
Write-Host ("{0} passed, {1} failed, {2} blocked. Workspace kept for inspection: {3}" -f @($results | Where-Object { $_.status -eq 'PASS' }).Count, $failed, $blocked, $ws)
exit $(if ($failed) { 1 } else { 0 })
