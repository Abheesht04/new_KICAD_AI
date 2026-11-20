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

    $g = Copper @('check', '-Intent', $intent)
    if ($g.exit -eq 0 -and $g.result.outcome -eq 'GROUNDED') { Step 'gate accepts the grounded example' 'PASS' 'GROUNDED' } else { Step 'gate accepts the grounded example' 'FAIL' $g.text }

    # Hallucination 1: the LED's polarity swapped in the model's mind (pin numbers valid, pin names wrong).
    $sw = Get-Content -Raw $intent | ConvertFrom-Json
    $d1 = $sw.components | Where-Object { $_.reference -eq 'D1' }
    $d1.pins[0].pin_name = 'A'; $d1.pins[1].pin_name = 'K'
    $swPath = Join-Path $ws 'designs\swapped\intent.json'; New-Item -ItemType Directory -Force (Split-Path $swPath) | Out-Null
    [IO.File]::WriteAllText($swPath, ($sw | ConvertTo-Json -Depth 8), (New-Object Text.UTF8Encoding($false)))
    $gs = Copper @('check', '-Intent', $swPath)
    if ($gs.exit -eq 2 -and $gs.text -match 'PIN_NAME_MISMATCH') { Step 'gate rejects swapped LED polarity' 'PASS' 'PIN_NAME_MISMATCH' } else { Step 'gate rejects swapped LED polarity' 'FAIL' $gs.text }
    $cs = Copper @('compile', '-Intent', $swPath, '-Out', (Join-Path $ws 'designs\swapped'))
    $nothing = -not (Get-ChildItem -Recurse -Filter '*.kicad_pcb' -Path (Join-Path $ws 'designs\swapped') -ErrorAction SilentlyContinue)
    if ($cs.exit -eq 2 -and $cs.result.outcome -eq 'REJECTED_UNGROUNDED' -and $nothing) { Step 'compile refuses an ungrounded intent' 'PASS' 'nothing compiled' } else { Step 'compile refuses an ungrounded intent' 'FAIL' $cs.text }

    # Hallucination 2: an invented part and an invented field.
    $iv = Get-Content -Raw $intent | ConvertFrom-Json
    ($iv.components | Where-Object { $_.reference -eq 'D1' }).symbol_id = 'Device:LED_UltraBright'
    $iv.components[0] | Add-Member -NotePropertyName mpn -NotePropertyValue 'XYZ-123'
    $ivPath = Join-Path $ws 'designs\invented\intent.json'; New-Item -ItemType Directory -Force (Split-Path $ivPath) | Out-Null
    [IO.File]::WriteAllText($ivPath, ($iv | ConvertTo-Json -Depth 8), (New-Object Text.UTF8Encoding($false)))
    $gi = Copper @('check', '-Intent', $ivPath)
    if ($gi.exit -eq 2 -and $gi.text -match 'SYMBOL_NOT_FOUND' -and $gi.text -match 'UNKNOWN_FIELD') { Step 'gate rejects invented part and field' 'PASS' 'SYMBOL_NOT_FOUND, UNKNOWN_FIELD' } else { Step 'gate rejects invented part and field' 'FAIL' $gi.text }

    $bad = Join-Path $ws 'designs\bad\intent.json'
    New-Item -ItemType Directory -Force (Split-Path $bad) | Out-Null
    $bj = Get-Content -Raw $intent | ConvertFrom-Json          # make D1 pin 2 a single-ended net: must be rejected
    ($bj.components | Where-Object { $_.reference -eq 'D1' }).pins[1].net = 'ONLY_ONE'
    [IO.File]::WriteAllText($bad, ($bj | ConvertTo-Json -Depth 8), (New-Object Text.UTF8Encoding($false)))
    $kb = Copper @('compile', '-Intent', $bad, '-Out', (Join-Path $ws 'designs\bad'))
    if ($kb.exit -eq 2 -and $kb.result.outcome -eq 'REJECTED_UNGROUNDED' -and $kb.text -match 'SINGLE_ENDED_NET') { Step 'compile rejects a single-ended net' 'PASS' 'gate: SINGLE_ENDED_NET, nothing compiled' } else { Step 'compile rejects a single-ended net' 'FAIL' $kb.text }

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

if ($ready) {
    $rp = Copper @('report', '-Project', $proj)
    $md = Join-Path $proj 'REPORT.md'
    if ($rp.exit -eq 0 -and (Test-Path $md) -and ((Get-Content -Raw $md) -match 'NOT ROUTED') -and ((Get-Content -Raw $md) -match 'GROUNDED')) { Step 'report (facts sheet)' 'PASS' 'REPORT.md: GROUNDED, NOT ROUTED, NOT APPROVED' } else { Step 'report (facts sheet)' 'FAIL' $rp.text }

    $hall = Join-Path $ws 'designs\led\answer_hallucinated.txt'
    [IO.File]::WriteAllText($hall, "Done! The board is fully routed and DRC passed with no errors and it is ready to manufacture. ERC is clean, DFM passed. I used MPN LTST-C191KRKT for the LED, verified against the datasheet.", (New-Object Text.UTF8Encoding($false)))
    $h = Copper @('claimcheck', '-Text', $hall, '-Project', $proj)
    $want = 'ROUTED', 'DRC_CLEAN', 'ERC_CLEAN', 'MANUFACTURING_READY', 'PART_DATA', 'VERIFIED'
    $missing = @($want | Where-Object { $h.text -notmatch "\[$_\]" })
    if ($h.exit -eq 1 -and -not $missing.Count) { Step 'claimcheck flags a hallucinated answer' 'PASS' ($want -join ', ') } else { Step 'claimcheck flags a hallucinated answer' 'FAIL' ("missing: " + ($missing -join ', ') + "`n" + $h.text) }

    $honest = Join-Path $ws 'designs\led\answer_honest.txt'
    [IO.File]::WriteAllText($honest, ((Get-Content -Raw $md) + "`nThe board is not routed and is not manufacturing-ready; the ERC and DRC findings above need review. No MPN was chosen."), (New-Object Text.UTF8Encoding($false)))
    $o = Copper @('claimcheck', '-Text', $honest, '-Project', $proj)
    if ($o.exit -eq 0) { Step 'claimcheck accepts an honest answer' 'PASS' 'no unsupported claims' } else { Step 'claimcheck accepts an honest answer' 'FAIL' $o.text }
}

# Analysis-only flow: Manus (a local mock: no account, no credits) returns copper.requirements for the owner's MOSFET
# prompt, including one value the prompt never states; Copper's C generator designs the board and ignores that value.
if ($ready) {
    $py = Join-Path $env:LOCALAPPDATA 'Programs\Python\Python312\python.exe'
    $mlog = Join-Path $ws 'mock_manus_log.json'
    $psi = New-Object Diagnostics.ProcessStartInfo $py, ('"' + (Join-Path $repo 'manus\tests\mock_manus_api.py') + '" "' + (Join-Path $repo 'manus\tests\mosfet_requirements.json') + '" "' + $mlog + '"')
    $psi.RedirectStandardOutput = $true; $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
    $mock = [Diagnostics.Process]::Start($psi)
    $port = ($mock.StandardOutput.ReadLine() -split ' ')[-1]
    $env:COPPER_MANUS_API_BASE = "http://127.0.0.1:$port"
    $ErrorActionPreference = 'Continue'
    $ownerPrompt = 'Design a compact 2-layer low-side MOSFET switch board for driving a 12 V DC load up to 2 A from a 3.3 V microcontroller GPIO. Include a screw terminal block for 12 V power and load, a 3-pin header for logic input, an AO3400A SOT-23 MOSFET, a 100 Ohm gate resistor, a 10 kOhm pull-down resistor, and a flyback diode across the load terminals. State your trace width and clearance assumptions.'
    try { $mp = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $ws 'copper-prompt.ps1') -Prompt $ownerPrompt -Name mosfet 2>&1 | ForEach-Object { "$_" } }
    finally { $mock.Kill(); Remove-Item Env:COPPER_MANUS_API_BASE }
    $mtext = $mp -join "`n"
    $requests = Get-Content -Raw $mlog | ConvertFrom-Json
    $allMock = -not @($requests | Where-Object { -not $_.key_is_mock }).Count
    if ($mtext -match 'GENERATED' -and $mtext -match 'IGNORED board_width_mm' -and $mtext -match 'gate_voltage_on\s+3\.267 V' -and $mtext -match 'GROUNDED' -and $mtext -match 'NOT ROUTED' -and $allMock) {
        Step 'prompt -> Manus analysis -> Copper design (mock)' 'PASS' 'analysis only; C generated the switch; invented value ignored; GROUNDED; real key never sent'
    } else { Step 'prompt -> Manus analysis -> Copper design (mock)' 'FAIL' (($mp | Select-Object -Last 30) -join "`n") }

    # copper.ps1 generate directly: an LED analysis, and one that omits the supply (Copper asks, never guesses).
    $ld = Join-Path $ws 'designs\led_gen'; New-Item -ItemType Directory -Force $ld | Out-Null
    [IO.File]::WriteAllText((Join-Path $ld 'prompt.txt'), 'A 5 V board with a red LED driven at 5 mA.', (New-Object Text.UTF8Encoding($false)))
    $req = '{"schema":"copper.requirements","schema_version":1,"circuit":"led_indicator","summary":"LED","supply_voltage_v":5,"load_current_a":null,"logic_voltage_v":null,"switch_part":null,"gate_resistor_ohm":null,"pulldown_ohm":null,"flyback_diode":null,"led_color":"red","led_current_ma":5,"led_forward_voltage_v":null,"board_width_mm":null,"board_height_mm":null,"evidence":[{"field":"supply_voltage_v","quote":"A 5 V board"},{"field":"led_current_ma","quote":"at 5 mA"},{"field":"led_color","quote":"a red LED"}],"open_questions":[],"unsupported_requests":[]}'
    [IO.File]::WriteAllText((Join-Path $ld 'requirements.json'), $req, (New-Object Text.UTF8Encoding($false)))
    $gl = Copper @('generate', '-Requirements', (Join-Path $ld 'requirements.json'), '-PromptFile', (Join-Path $ld 'prompt.txt'), '-Out', $ld)
    if ($gl.exit -eq 0 -and $gl.text -match '"value": "620"' -and (Test-Path (Join-Path $ld 'intent.json'))) { Step 'generate: LED indicator' 'PASS' '620 ohm (E24, rounded up), Vf assumed and UNVERIFIED' } else { Step 'generate: LED indicator' 'FAIL' $gl.text }
    $nd = Join-Path $ws 'designs\led_missing'; New-Item -ItemType Directory -Force $nd | Out-Null
    [IO.File]::WriteAllText((Join-Path $nd 'prompt.txt'), 'A red LED indicator.', (New-Object Text.UTF8Encoding($false)))
    [IO.File]::WriteAllText((Join-Path $nd 'requirements.json'), ($req -replace '"supply_voltage_v":5', '"supply_voltage_v":null'), (New-Object Text.UTF8Encoding($false)))
    $gn = Copper @('generate', '-Requirements', (Join-Path $nd 'requirements.json'), '-PromptFile', (Join-Path $nd 'prompt.txt'), '-Out', $nd)
    if ($gn.exit -eq 3 -and $gn.result.outcome -eq 'NEEDS_INPUT' -and $gn.text -match 'What is the supply voltage' -and -not (Test-Path (Join-Path $nd 'intent.json'))) { Step 'generate: missing supply -> question' 'PASS' 'NEEDS_INPUT, nothing designed' } else { Step 'generate: missing supply -> question' 'FAIL' $gn.text }
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
