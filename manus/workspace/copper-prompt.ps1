<#
copper-prompt.ps1: write your own design prompts in the terminal.

    powershell -ExecutionPolicy Bypass -File .\copper-prompt.ps1                      # interactive: type prompts
    powershell -ExecutionPolicy Bypass -File .\copper-prompt.ps1 -Prompt "low-side MOSFET switch for a 12 V, 2 A load from 3.3 V logic"
    powershell -ExecutionPolicy Bypass -File .\copper-prompt.ps1 -PromptFile my_request.txt [-Profile lite|standard|max]

Division of work (owner decision 2026-11-20): Manus is used ONLY to analyse the prompt. It returns copper.requirements
v1: circuit type, voltages, currents, named parts and options, each with a verbatim quote from your prompt, plus open
questions. It never chooses parts, pins, nets or values. Everything else is Copper's own code:
  1. Manus (https://api.manus.ai, task.create, interactive_mode false, structured_output_schema) -> requirements.json
  2. `copper design generate` (C, src/v2_src/template_gen.c): checks each value against a quote in your prompt
     (unbacked values are IGNORED and reported), picks the circuit template (design_templates/), resolves every pin
     from your KiCad libraries and computes values with integer formulas -> intent.json + generation.json
  3. `copper design check-intent` (C grounding gate) -> GROUNDED or nothing is compiled
  4. compile (KiCad project, netlist, ERC, DRC, Copper import) and REPORT.md (facts from tool output only)
A request no template covers may be answered with a composition of Copper's circuit blocks (circuit "compose"):
`copper design compose` (C, src/v2_src/compose.c) checks it, every value against its quote, then the grounding gate;
composed designs are not yet placed, compiled or routed (Step 2e).
Missing information comes back as Copper's questions; a circuit without a template is NOT_IMPLEMENTED. Everything is
saved under designs\<name>_<time>\. Each prompt uses Manus credits on your account (one analysis task).

Key: manus_api_key.txt in this workspace folder (paste it there in an editor; the file is outside git), else the one
stored by manus\set_manus_api_key.ps1 (%APPDATA%\Copper\manus_api_key.dpapi). It is sent only to
https://api.manus.ai; with COPPER_MANUS_API_BASE pointing anywhere else (tests), a dummy key is sent instead.
#>
[CmdletBinding()]
param(
    [string]$Prompt,
    [string]$PromptFile,
    [string]$Name,
    [ValidateSet('lite', 'standard', 'max')] [string]$Profile = 'standard',
    [ValidateRange(1, 120)] [int]$TimeoutMinutes = 20
)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$cfg = Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $here 'copper.config.json') | ConvertFrom-Json
$official = 'https://api.manus.ai'
$base = if ($env:COPPER_MANUS_API_BASE) { $env:COPPER_MANUS_API_BASE.TrimEnd('/') } else { $official }
$utf8 = New-Object Text.UTF8Encoding($false)

function Say([string]$text, [string]$color = 'Gray') { Write-Host $text -ForegroundColor $color }

function Get-ApiKey {
    if ($base -ne $official) { return 'mock-key' }   # never send the real key anywhere but Manus
    # 1. manus_api_key.txt next to this script (the workspace folder, outside git): edit it in any editor.
    $txt = Join-Path $here 'manus_api_key.txt'
    if (Test-Path -LiteralPath $txt) {
        foreach ($line in Get-Content -Encoding UTF8 -LiteralPath $txt) {
            $k = $line.Trim()
            if ($k -and -not $k.StartsWith('#') -and $k -ne 'PASTE-YOUR-MANUS-API-KEY-HERE') { return $k }
        }
    }
    # 2. The DPAPI store written by manus\set_manus_api_key.ps1.
    $file = Join-Path $env:APPDATA 'Copper\manus_api_key.dpapi'
    if (-not (Test-Path -LiteralPath $file)) { throw "No Manus API key. Paste it into $txt (replace the placeholder line) or run manus\set_manus_api_key.ps1" }
    $secure = (Get-Content -Raw -LiteralPath $file).Trim() | ConvertTo-SecureString
    $ptr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try { return [Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr) } finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr) }
}

function Manus([string]$method, [string]$path, $body) {
    $headers = @{ 'x-manus-api-key' = (Get-ApiKey) }
    $uri = "$base/v2/$path"
    for ($attempt = 1; ; $attempt++) {
        try {
            if ($method -eq 'GET') { return Invoke-RestMethod -Method Get -Uri $uri -Headers $headers -TimeoutSec 60 }
            $bytes = $utf8.GetBytes(($body | ConvertTo-Json -Depth 40 -Compress))
            return Invoke-RestMethod -Method Post -Uri $uri -Headers $headers -ContentType 'application/json; charset=utf-8' -Body $bytes -TimeoutSec 60
        } catch {
            $status = 0
            $detail = if ($_.ErrorDetails -and $_.ErrorDetails.Message) { $_.ErrorDetails.Message } else { $_.Exception.Message }
            if ($_.Exception.Response) { $status = [int]$_.Exception.Response.StatusCode }
            if ($status -eq 429 -and $attempt -le 5) { Start-Sleep -Seconds ([Math]::Min(60, 5 * $attempt + (Get-Random -Maximum 3))); continue }
            if ($status -eq 401) {
                throw "Manus rejected the API key (HTTP 401): $detail`n  Create a new key at https://manus.im/app#settings/developers (it is shown only once), then paste it into $(Join-Path $here 'manus_api_key.txt')"
            }
            throw "Manus API $path failed (HTTP $status): $detail"
        }
    }
}

function All-Events([string]$taskId) {
    $events = @(); $cursor = $null
    do {
        $q = "task.listMessages?task_id=$taskId&order=asc&limit=200" + $(if ($cursor) { "&cursor=$cursor" } else { '' })
        $r = Manus 'GET' $q $null
        $events += @($r.messages)
        $cursor = if ($r.has_more) { $r.next_cursor } else { $null }
    } while ($cursor)
    return $events
}

# Wait for the analysis (a structured_output_result). Questions from Manus are answered with "continue".
function Wait-Analysis([string]$taskId, [string]$runDir) {
    $deadline = (Get-Date).AddMinutes($TimeoutMinutes)
    $nudges = 0; $lastStatus = ''
    while ((Get-Date) -lt $deadline) {
        $events = All-Events $taskId
        [IO.File]::WriteAllText((Join-Path $runDir 'manus_events.json'), (ConvertTo-Json @($events) -Depth 40), $utf8)
        $status = ($events | Where-Object { $_.type -eq 'status_update' } | Select-Object -Last 1).status_update
        $agent = if ($status) { $status.agent_status } else { 'running' }
        if ($agent -ne $lastStatus) { Say "  Manus: $agent" 'DarkGray'; $lastStatus = $agent }
        $result = $events | Where-Object { $_.type -eq 'structured_output_result' } | Select-Object -Last 1
        if ($result -and $agent -ne 'running') {
            $detail = (Manus 'GET' "task.detail?task_id=$taskId" $null).task
            if (-not $detail.has_running_background_jobs) { return $result }
        }
        if ($agent -eq 'error') { throw "Manus reported an error: $((($events | Where-Object { $_.type -eq 'error_message' } | Select-Object -Last 1)) | ConvertTo-Json -Depth 10 -Compress)" }
        if ($agent -eq 'stopped' -and -not $result) { throw 'Manus stopped without returning the analysis; see manus_events.json' }
        if ($agent -eq 'waiting') {
            if ($nudges -ge 2) { throw 'Manus keeps waiting for input; open the task in Manus to answer it.' }
            $nudges++
            Manus 'POST' 'task.sendMessage' @{ task_id = $taskId; message = @{ content = 'Continue without asking: put anything unclear in open_questions and leave unstated values null.' } } | Out-Null
        }
        Start-Sleep -Seconds 5
    }
    throw "No analysis from Manus within $TimeoutMinutes minutes (task $taskId)."
}

function Run-Copper([string]$exe, [string[]]$argv) {
    $out = & $exe @argv 2>&1 | ForEach-Object { "$_" }
    return [pscustomobject]@{ exit = $LASTEXITCODE; text = ($out -join "`n") }
}

# The block list for Manus's instructions, from `copper block list` (C); formatting only.
function Block-Catalogue {
    $files = @(Get-ChildItem -LiteralPath $cfg.blocks_dir -Filter '*.block.json' | Sort-Object Name | ForEach-Object { $_.FullName })
    if (-not $files.Count) { return '(no blocks installed)' }
    $r = Run-Copper $cfg.copper (@('block', 'list') + $files)
    if ($r.exit -ne 0) { throw "copper block list failed:`n$($r.text)" }
    $list = $r.text | ConvertFrom-Json
    $lines = foreach ($b in @($list.blocks)) {
        $ports = (@($b.ports) | ForEach-Object {
            $range = if ($null -ne $_.voltage_min) { ' {0}-{1} uV' -f $_.voltage_min, $_.voltage_max } else { '' }
            '{0}({1}{2})' -f $_.name, $_.kind, $range }) -join ', '
        $params = (@($b.parameters) | ForEach-Object {
            $how = if ($_.derived_from_connection) { 'derived' } elseif ($_.required) { 'required' } else { "default $($_.default)" }
            '{0} {1} {2}' -f $_.name, $_.unit, $how }) -join '; '
        "- $($b.name): $($b.summary)`n    ports: $ports`n    parameters: $(if ($params) { $params } else { 'none' })"
    }
    return ($lines -join "`n")
}

# Manus's composition (blocks, parameter lists, nets, board, evidence) -> copper.compose/1. A plain mapping: every
# check is Copper's composer (C), which also checks each value against its quote in the prompt.
function Write-Compose($analysis, [string]$name, [string]$path) {
    $comp = $analysis.composition
    $project = [ordered]@{ name = $name }
    if ($null -ne $comp.board_width_mm -and $null -ne $comp.board_height_mm) {
        $project['board'] = [ordered]@{ width_mm = [int64]$comp.board_width_mm; height_mm = [int64]$comp.board_height_mm }
    }
    $blocks = @(@($comp.blocks) | ForEach-Object {
        $id = $_.id
        $p = [ordered]@{}
        foreach ($kv in @($comp.parameters | Where-Object { $_.block_id -eq $id })) { $p[[string]$kv.name] = [int64]$kv.value }
        [ordered]@{ id = $id; block = $_.block; parameters = $p } })
    # a parameter for a block id that does not exist is passed on, so the composer rejects it (never dropped silently)
    foreach ($kv in @($comp.parameters | Where-Object { $bid = $_.block_id; -not @($comp.blocks | Where-Object { $_.id -eq $bid }).Count })) {
        $blocks += [ordered]@{ id = [string]$kv.block_id; block = '(unknown block for a parameter)'; parameters = [ordered]@{ ([string]$kv.name) = [int64]$kv.value } }
    }
    $nets = @(@($comp.nets) | ForEach-Object {
        $n = [ordered]@{}
        if ($_.name) { $n['name'] = $_.name }
        $n['ports'] = @($_.ports)
        $n })
    $doc = [ordered]@{ format = 'copper.compose/1'; project = $project; blocks = $blocks; nets = $nets; evidence = @($comp.evidence) }
    [IO.File]::WriteAllText($path, ($doc | ConvertTo-Json -Depth 20), $utf8)
}

function Run-Prompt([string]$text) {
    $slug = if ($Name) { $Name } else { (($text -replace '[^A-Za-z0-9]+', '_').Trim('_')) }
    if ($slug.Length -gt 32) { $slug = $slug.Substring(0, 32).Trim('_') }
    if (-not $slug) { $slug = 'design' }
    $runDir = Join-Path $here ("designs\{0}_{1}" -f $slug, (Get-Date -Format 'yyyyMMdd_HHmmss'))
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null
    $promptPath = Join-Path $runDir 'prompt.txt'
    [IO.File]::WriteAllText($promptPath, $text, $utf8)
    Say ''; Say "== $slug  ($runDir)" 'Cyan'

    # 1. Manus: prompt analysis ONLY.
    $schema = Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $here 'manus_requirements.schema.json') | ConvertFrom-Json
    $blockText = Block-Catalogue
    $instructions = @"
You are the prompt-analysis step of Copper, a local PCB engineering tool. Your ONLY job is to read the user's request
and fill in the structured output (copper.requirements v1). You do NOT design anything: never choose parts, pins,
nets, footprints, component values or layouts, and never claim anything was checked, routed or is ready. Copper's own
code generates the design from your analysis.
Rules:
- circuit: "low_side_switch" (a MOSFET/transistor switching a load from a logic signal), "esp32_led" (an ESP32 board
  with an LED), "led_indicator" (an LED with a series resistor and nothing else). Pick the circuit that includes
  EVERYTHING the user asked for. If none does but the request can be built from the BLOCKS below, use "compose" and
  fill "composition"; otherwise use "other" (and say what was asked in summary). For any circuit other than "compose",
  give "composition" empty lists and null board sizes.
- composition (only for "compose"): choose blocks from the list below and connect their ports. You choose WHICH blocks
  and WHICH ports join; you never choose parts, pins or component values (each block brings its own, and Copper
  checks every voltage, current and connection and refuses what does not fit):
  - blocks: [{id, block}]; id is a short lower-case name you give (for example "mcu", "led1"); block is a block name
    from the list.
  - parameters: [{block_id, name, value}] ONLY for values the user stated, as integers in the parameter's unit (uV, uA,
    pF, ...: 5 V -> 5000000). Never give a parameter marked "derived".
  - nets: [{name, ports}]; ports are "id.port"; join every ground port in one net, connect every power_in to exactly
    one power_out, and a pin of the user's choice (or any free GPIO) to each LED or sensor. name may be null.
  - board_width_mm / board_height_mm: only if the user stated the board size; otherwise null.
  - evidence: for every parameter you give, {field: "id.name", quote: exact words from the request}; for the board,
    fields "project.board.width_mm" and "project.board.height_mm".
- Fill a value ONLY if the user stated it. Otherwise leave it null; do not estimate, assume or calculate.
- For every non-null value add an evidence item: field = the field name, quote = the exact words from the request that
  state it, copied verbatim (Copper checks that the quote occurs in the request and contains the value).
- Units: volts, amps, ohms (10 kOhm -> 10000), milliamps for led_current_ma, millimetres for the board.
- switch_part: a part number the user named for the switch (for example "AO3400A"), exactly as written.
- open_questions: anything unclear or missing that matters for the design.
- unsupported_requests: anything asked that the fields cannot express (for example trace width or clearance
  assumptions, extra connectors, enclosure, firmware).

BLOCKS Copper can compose (name: summary; ports as name(kind); parameters as name unit, required / derived / default):
$blockText

USER REQUEST:
$text
"@
    [IO.File]::WriteAllText((Join-Path $runDir 'manus_prompt.txt'), $instructions, $utf8)
    Say "1. Manus analyses the prompt ($Profile profile; analysis only)..."
    $create = Manus 'POST' 'task.create' @{
        message = @{ content = $instructions }
        interactive_mode = $false
        agent_profile = $Profile
        title = "Copper analysis: $slug"
        structured_output_schema = $schema
    }
    $taskId = $create.task_id
    [IO.File]::WriteAllText((Join-Path $runDir 'manus_task.json'), ($create | ConvertTo-Json -Depth 5), $utf8)
    Say "   task $taskId  $($create.task_url)" 'DarkGray'
    $ev = Wait-Analysis $taskId $runDir
    $sor = $ev.structured_output_result
    if (-not $sor.success) { throw "Manus could not produce the analysis: $($sor.error | ConvertTo-Json -Compress)" }
    $analysis = if ($sor.value -is [string]) { $sor.value | ConvertFrom-Json } else { $sor.value }
    $reqPath = Join-Path $runDir 'requirements.json'
    [IO.File]::WriteAllText($reqPath, ($analysis | ConvertTo-Json -Depth 20), $utf8)
    $taskDetail = (Manus 'GET' "task.detail?task_id=$taskId" $null).task
    Say ("   circuit: {0}  ({1})" -f $analysis.circuit, $analysis.summary)
    $manusNotes = @(@($analysis.open_questions) | ForEach-Object { "- Open question (Manus): $_" }) + @(@($analysis.unsupported_requests) | ForEach-Object { "- Not handled by Copper (Manus): $_" })

    # 2b. A composition from blocks: Copper's composer (C) checks it, the gate, the C placer, then compile and report.
    if ($analysis.circuit -eq 'compose') {
        if ($null -eq $analysis.composition -or -not @($analysis.composition.blocks).Count) { Say 'RESULT: Manus chose "compose" but gave no blocks - nothing was designed.' 'Yellow'; return }
        $designPath = Join-Path $runDir 'design.compose.json'
        $projName = (($slug -replace '[^A-Za-z0-9_-]', '_'))
        if ($projName.Length -gt 40) { $projName = $projName.Substring(0, 40) }
        Write-Compose $analysis $projName $designPath
        Say '2. Copper composes the design from blocks (deterministic, C)...'
        $copperPs1 = Join-Path $here 'copper.ps1'
        $c = & powershell -NoProfile -ExecutionPolicy Bypass -File $copperPs1 compose -Design $designPath -PromptFile $promptPath -Out $runDir 2>&1 | ForEach-Object { "$_" }
        $cr = ($c | Where-Object { $_ -like 'COPPER_RESULT *' } | Select-Object -Last 1)
        $res = if ($cr) { $cr.Substring(14) | ConvertFrom-Json } else { $null }
        $rep = Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $runDir 'compose_report.json') | ConvertFrom-Json
        Say "   $($rep.status)$(if ($res -and $res.gate) { "; grounding gate: $($res.gate)" })" $(if ($res -and $res.exit -eq 0) { 'Green' } else { 'Yellow' })
        foreach ($v in @($rep.values)) { Say ("   {0,-28} {1}  [{2}]" -f $v.where, $v.value, $v.origin) 'DarkGray' }
        foreach ($f in @($rep.findings | Where-Object { $_.level -ne 'INFO' -or $_.code -eq 'IGNORED_VALUE' })) {
            Say ("   [{0}] {1} {2}: {3}" -f $f.level, $f.code, $f.where, $f.message) 'Yellow'
        }
        $lines = @(@($rep.values) | ForEach-Object { "- {0}: {1} ({2}; {3})" -f $_.where, $_.value, $_.origin, $_.source }) +
                 @(@($rep.findings) | ForEach-Object { "- [{0}] {1} {2}: {3}" -f $_.level, $_.code, $_.where, $_.message })
        $reportText = ''
        if ($res -and $res.exit -eq 0 -and $res.placed) {
            # Placed: the same compile, route, KiCad checks and facts as a template design.
            Say '3. Placed by the C placer; grounding gate, compile, route, KiCad/Copper checks...'
            $cc = & powershell -NoProfile -ExecutionPolicy Bypass -File $copperPs1 compile -Intent (Join-Path $runDir 'intent.json') -Out $runDir 2>&1 | ForEach-Object { "$_" }
            Say "   $(($cc | Where-Object { $_ -like 'COPPER_RESULT *' } | Select-Object -Last 1))" 'DarkGray'
            $proj = Join-Path $runDir $projName
            if (Test-Path -LiteralPath (Join-Path $proj "$projName.kicad_pcb")) {
                & powershell -NoProfile -ExecutionPolicy Bypass -File $copperPs1 report -Project $proj 2>&1 | Out-Null
                $reportText = Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $proj 'REPORT.md')
            }
        }
        $state = if ($reportText) { "COMPOSED, GROUNDED, placed and compiled: $($rep.components) components. See the facts below; nothing is manufacturing-ready." }
                 elseif ($res -and $res.exit -eq 0) { "COMPOSED and GROUNDED: $($rep.components) components, but not compiled (see above)." }
                 else { "$($rep.status)$(if ($res -and $res.outcome -and $res.outcome -ne $rep.status) { " / $($res.outcome)" }): nothing was designed." }
        $answer = $(if ($reportText) { $reportText + "`n" } else { '' }) + "# Copper composition`n`n- Result: $state`n`n## Values and findings (Copper)`n`n" + ($lines -join "`n") +
                  "`n`n## Manus prompt analysis notes (not checked by Copper)`n`n" + $(if ($manusNotes.Count) { $manusNotes -join "`n" } else { '- none' }) + "`n"
        if ($reportText) { Say ''; Say '================ RESULT ================' 'Cyan'; $reportText -split "`n" | Where-Object { $_ -match '^- ' } | ForEach-Object { Say $_ } }
        [IO.File]::WriteAllText((Join-Path $runDir 'answer.md'), $answer, $utf8)
        Say ''; Say "RESULT: $state" $(if ($res -and $res.exit -eq 0) { 'Cyan' } else { 'Yellow' })
        if ($rep.status -eq 'NEEDS_INPUT') { Say 'Add the missing information to your prompt and run it again. Copper does not guess.' 'DarkGray' }
        if ($manusNotes.Count) { Say 'Manus analysis notes:'; $manusNotes | ForEach-Object { Say "  $_" } }
        Say "Files:   $runDir" 'Gray'
        Say ("Manus:   {0}   credits used: {1}" -f $create.task_url, $(if ($null -ne $taskDetail.credit_usage) { $taskDetail.credit_usage } else { 'n/a' })) 'Gray'
        return
    }

    # 2. Copper generates the design (C template generator; evidence checked against the prompt).
    Say '2. Copper generates the design from the analysis (deterministic, C)...'
    $g = Run-Copper $cfg.copper @('design', 'generate', $reqPath, '--templates', $cfg.templates, '--symbol-root', $cfg.symbols_root,
                                  '--footprint-root', $cfg.footprints_root, '--prompt', $promptPath, '--out', $runDir)
    $gen = Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $runDir 'generation.json') | ConvertFrom-Json
    Say "   $($gen.status)" $(if ($g.exit -eq 0) { 'Green' } else { 'Yellow' })
    foreach ($v in @($gen.values)) { Say ("   {0,-22} {1,-12} [{2}] {3}" -f $v.name, $v.value, $v.provenance, $v.note) 'DarkGray' }
    foreach ($i in @($gen.ignored_requirements)) { Say ("   IGNORED {0}: {1}" -f $i.field, $i.reason) 'Yellow' }

    if ($g.exit -ne 0) {
        Say ''; Say "RESULT: $($gen.status) - nothing was designed." 'Yellow'
        foreach ($q in @($gen.questions)) { Say "  Copper needs: $q" 'Yellow' }
        foreach ($c in @($gen.checks | Where-Object { $_.status -ne 'INFO' })) { Say "  [$($c.status)] $($c.message)" 'Yellow' }
        if ($manusNotes.Count) { Say 'Manus analysis notes:'; $manusNotes | ForEach-Object { Say "  $_" } }
        if ($gen.status -eq 'NEEDS_INPUT') { Say 'Add the missing information to your prompt and run it again.' 'DarkGray' }
        elseif ($gen.status -eq 'NOT_IMPLEMENTED') { Say 'Copper cannot build this request yet (no template covers all of it). Nothing was reduced or guessed.' 'DarkGray' }
        return
    }

    # 3. Grounding gate + compile + checks + facts.
    Say '3. Grounding gate, compile, KiCad/Copper checks...'
    $copperPs1 = Join-Path $here 'copper.ps1'
    $c = & powershell -NoProfile -ExecutionPolicy Bypass -File $copperPs1 compile -Intent (Join-Path $runDir 'intent.json') -Out $runDir 2>&1 | ForEach-Object { "$_" }
    $cr = ($c | Where-Object { $_ -like 'COPPER_RESULT *' } | Select-Object -Last 1)
    Say "   $cr" 'DarkGray'
    $projName = (Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $runDir 'intent.json') | ConvertFrom-Json).project.name
    $proj = Join-Path $runDir $projName
    if (-not (Test-Path -LiteralPath (Join-Path $proj "$projName.kicad_pcb"))) {
        Say ''; Say 'RESULT: the gate or the compiler stopped the design (see above).' 'Red'
        $c | Select-Object -Last 15 | ForEach-Object { Say "  $_" 'DarkGray' }
        return
    }
    & powershell -NoProfile -ExecutionPolicy Bypass -File $copperPs1 report -Project $proj 2>&1 | Out-Null
    $report = Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $proj 'REPORT.md')
    $genLines = @(@($gen.values) | ForEach-Object { "- {0}: {1} ({2}: {3})" -f $_.name, $_.value, $_.provenance, $_.note }) +
                @(@($gen.checks) | ForEach-Object { "- [{0}] {1}" -f $_.status, $_.message })
    $answer = $report + "`n## How Copper generated it (template $($gen.template))`n`n" + ($genLines -join "`n") +
              "`n`n## Manus prompt analysis notes (not checked by Copper)`n`n" + $(if ($manusNotes.Count) { $manusNotes -join "`n" } else { '- none' }) + "`n"
    [IO.File]::WriteAllText((Join-Path $runDir 'answer.md'), $answer, $utf8)

    Say ''; Say '================ RESULT ================' 'Cyan'
    $report -split "`n" | Where-Object { $_ -match '^- ' } | ForEach-Object { Say $_ }
    Say ''; Say "Generated by Copper from template '$($gen.template)':"
    foreach ($ck in @($gen.checks)) { Say ("  [{0}] {1}" -f $ck.status, $ck.message) $(if ($ck.status -eq 'INFO') { 'Gray' } else { 'Yellow' }) }
    Say ''; Say 'Manus prompt analysis notes (not checked by Copper):'
    if ($manusNotes.Count) { $manusNotes | ForEach-Object { Say "  $_" } } else { Say '  none' }
    Say ''
    Say "Files:   $proj" 'Gray'
    Say "Answer:  $runDir\answer.md" 'Gray'
    Say ("Manus:   {0}   credits used: {1}" -f $create.task_url, $(if ($null -ne $taskDetail.credit_usage) { $taskDetail.credit_usage } else { 'n/a' })) 'Gray'
}

$text = $Prompt
if ($PromptFile) { $text = Get-Content -Raw -Encoding UTF8 -LiteralPath $PromptFile }
if ($text) { Run-Prompt $text.Trim(); exit 0 }

Say 'Copper prompt console: type a design request and press Enter (empty line to quit).' 'Cyan'
Say "Manus only analyses each request ($Profile profile, uses credits); Copper's own code designs, checks and reports." 'DarkGray'
Say 'Copper can currently generate: low-side MOSFET switch, LED indicator, ESP32 board with an LED (templates), and compose ESP32-S3 designs from blocks (checked, not yet compiled). Other circuits are reported as NOT_IMPLEMENTED.' 'DarkGray'
while ($true) {
    Write-Host ''
    $line = Read-Host 'Prompt'
    if (-not $line -or -not $line.Trim()) { break }
    try { Run-Prompt $line.Trim() } catch { Say "ERROR: $($_.Exception.Message)" 'Red' }
}
