<#
copper-prompt.ps1: write your own design prompts in the terminal; Manus turns each one into a design intent through
the Manus API, and Copper checks, compiles and reports it on this PC.

    powershell -ExecutionPolicy Bypass -File .\copper-prompt.ps1                      # interactive: type prompts
    powershell -ExecutionPolicy Bypass -File .\copper-prompt.ps1 -Prompt "5 V LED indicator with a 2-pin header"
    powershell -ExecutionPolicy Bypass -File .\copper-prompt.ps1 -PromptFile my_request.txt [-Profile lite|standard|max]

For each prompt:
  1. parts list: the real pin numbers, pin names and pads of candidate parts from YOUR installed KiCad libraries
     (or exactly the parts the prompt names as "symbol Lib:Name footprint Lib:Name");
  2. Manus (https://api.manus.ai, task.create, interactive_mode false, structured_output_schema) writes a
     copper.design-intent, plus its assumptions and open questions as separate fields;
  3. Copper's grounding gate checks every part, pin, pin name, pad and net against the installed libraries. If it
     REJECTS the intent, the findings go back to the same Manus task (task.sendMessage), at most -MaxRepairs times;
  4. only a GROUNDED intent is compiled (KiCad project, netlist, ERC, DRC, Copper import);
  5. REPORT.md (facts from tool output only); Manus's assumptions and questions are shown separately, marked
     "not checked by Copper", and run through claimcheck.
Everything is saved under designs\<name>_<time>\. Each prompt uses Manus credits on your account.

Key: the one stored by manus\set_manus_api_key.ps1 (%APPDATA%\Copper\manus_api_key.dpapi). It is sent only to
https://api.manus.ai; with COPPER_MANUS_API_BASE pointing anywhere else (tests), a dummy key is sent instead.
#>
[CmdletBinding()]
param(
    [string]$Prompt,
    [string]$PromptFile,
    [string]$Name,
    [ValidateSet('lite', 'standard', 'max')] [string]$Profile = 'standard',
    [ValidateRange(0, 5)] [int]$MaxRepairs = 2,
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
    $file = Join-Path $env:APPDATA 'Copper\manus_api_key.dpapi'
    if (-not (Test-Path -LiteralPath $file)) { throw 'No Manus API key stored. Run: manus\set_manus_api_key.ps1' }
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
            $status = 0; $detail = $_.Exception.Message
            if ($_.Exception.Response) {
                $status = [int]$_.Exception.Response.StatusCode
                try { $detail = (New-Object IO.StreamReader($_.Exception.Response.GetResponseStream())).ReadToEnd() } catch { }
            }
            if ($status -eq 429 -and $attempt -le 5) { Start-Sleep -Seconds ([Math]::Min(60, 5 * $attempt + (Get-Random -Maximum 3))); continue }
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

# Wait until Manus delivers a structured_output_result that was not seen before. Returns the event.
function Wait-Result([string]$taskId, [hashtable]$seen, [string]$runDir) {
    $deadline = (Get-Date).AddMinutes($TimeoutMinutes)
    $nudges = 0; $lastStatus = ''
    while ((Get-Date) -lt $deadline) {
        $events = All-Events $taskId
        [IO.File]::WriteAllText((Join-Path $runDir 'manus_events.json'), (ConvertTo-Json @($events) -Depth 40), $utf8)
        $status = ($events | Where-Object { $_.type -eq 'status_update' } | Select-Object -Last 1).status_update
        $agent = if ($status) { $status.agent_status } else { 'running' }
        if ($agent -ne $lastStatus) { Say "  Manus: $agent" 'DarkGray'; $lastStatus = $agent }
        $fresh = $events | Where-Object { $_.type -eq 'structured_output_result' -and -not $seen.ContainsKey($_.id) } | Select-Object -Last 1
        if ($fresh -and $agent -ne 'running') {
            $detail = (Manus 'GET' "task.detail?task_id=$taskId" $null).task
            if (-not $detail.has_running_background_jobs) { $seen[$fresh.id] = $true; return $fresh }
        }
        if ($agent -eq 'error') {
            $err = ($events | Where-Object { $_.type -eq 'error_message' } | Select-Object -Last 1)
            throw "Manus reported an error: $(($err | ConvertTo-Json -Depth 10 -Compress))"
        }
        if ($agent -eq 'stopped' -and -not $fresh) {
            throw 'Manus stopped without a structured result; see manus_events.json'
        }
        if ($agent -eq 'waiting') {
            if ($nudges -ge 2) { throw 'Manus keeps waiting for input; open the task in Manus to answer it.' }
            $nudges++
            Manus 'POST' 'task.sendMessage' @{ task_id = $taskId; message = @{ content = 'Continue and use your best judgment. Do not ask questions: record anything unresolved in open_questions, and only use parts from the parts list.' } } | Out-Null
        }
        Start-Sleep -Seconds 5
    }
    throw "No result from Manus within $TimeoutMinutes minutes (task $taskId)."
}

function Gate([string]$intentPath, [string]$reportPath) {
    $out = & $cfg.copper @('design', 'check-intent', $intentPath, '--symbol-root', $cfg.symbols_root, '--footprint-root', $cfg.footprints_root, '--out', $reportPath) 2>&1 | ForEach-Object { "$_" }
    return [pscustomobject]@{ exit = $LASTEXITCODE; text = ($out -join "`n") }
}

function Run-Prompt([string]$text) {
    $slug = if ($Name) { $Name } else { (($text -replace '[^A-Za-z0-9]+', '_').Trim('_')) }
    if ($slug.Length -gt 32) { $slug = $slug.Substring(0, 32).Trim('_') }
    if (-not $slug) { $slug = 'design' }
    $runDir = Join-Path $here ("designs\{0}_{1}" -f $slug, (Get-Date -Format 'yyyyMMdd_HHmmss'))
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null
    [IO.File]::WriteAllText((Join-Path $runDir 'prompt.txt'), $text, $utf8)
    Say ''; Say "== $slug  ($runDir)" 'Cyan'

    # 1. Parts list from the installed libraries (Copper Lab's tested lookup: named parts, or standard candidates).
    Say '1. Reading real part data from your KiCad libraries...'
    $promptFile = Join-Path $runDir 'prompt.txt'
    $ctxFile = Join-Path $runDir 'parts_context.json'
    $py = "import json, sys; sys.path.insert(0, sys.argv[1]); import design_pipeline as d; from pathlib import Path; " +
          "t = Path(sys.argv[2]).read_text(encoding='utf-8'); c = d.collect_asset_context(t, Path(sys.argv[3]), Path(sys.argv[4])); " +
          "d.reject_explicitly_unavailable_assets(t, c); Path(sys.argv[5]).write_text(json.dumps(c, indent=1), encoding='utf-8')"
    & $cfg.python -c $py (Join-Path $cfg.repo 'copper_lab') $promptFile $cfg.symbols_root $cfg.footprints_root $ctxFile 2>&1 | ForEach-Object { Say "   $_" 'DarkGray' }
    if ($LASTEXITCODE -ne 0) { throw 'The parts lookup failed (see above). Name parts as "symbol Lib:Name footprint Lib:Name" or use standard ones.' }
    $ctx = Get-Content -Raw -Encoding UTF8 -LiteralPath $ctxFile | ConvertFrom-Json
    $menu = [ordered]@{
        symbols = @($ctx.symbols | ForEach-Object { [ordered]@{ id = $_.id; pins = @($_.pins | ForEach-Object { [ordered]@{ number = $_.number; name = $_.name; type = $_.electrical_type } }) } })
        footprints = @($ctx.footprints | ForEach-Object { [ordered]@{ id = $_.id; pads = @($_.pads | ForEach-Object { $_.number } | Where-Object { $_ } | Select-Object -Unique) } })
        candidate_pairs = @($ctx.candidate_pairs)
        not_usable = @($ctx.unavailable_candidates)
    }
    Say ("   {0} symbol(s), {1} footprint(s)" -f @($menu.symbols).Count, @($menu.footprints).Count)

    # 2. Manus writes the design intent (structured output).
    $schema = Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $here 'manus_output.schema.json') | ConvertFrom-Json
    $instructions = @"
You are the design-intent author for Copper, a local PCB engineering tool. Copper (not you) checks, compiles and validates.
Turn the user's request into a copper.design-intent v1 in the structured output. Rules:
- Use ONLY symbol and footprint IDs from the PARTS LIST below, as symbol/footprint pairs (candidate_pairs when given). Never use not_usable parts.
- For every pin: symbol_pin = the pin number, pin_name = that pin's name EXACTLY as in the parts list ("" for unnamed), pad = a pad number of the chosen footprint. Map every symbol pin and every footprint pad exactly once.
- LEDs/diodes: the pin named K is the cathode, A is the anode. Do not assume pin numbers.
- Every named net connects at least two pins; use null only for an intentional no-connect. Never leave a power input pin unconnected.
- project.name: letters, digits, _ or - only. Board: 2 layers, width 20-300 mm, height 15-300 mm. Every component gets a position at least 2.5 mm inside the board edges, spaced so footprints do not overlap.
- References like R1, D1, J1, C1. value: a short value such as 1k, 100n, RED, 5V_IN.
- Do not invent part numbers, ratings, prices or availability, and do not claim the board is routed, checked or ready. Put every assumption you made (supply voltage, LED current, values chosen) in "assumptions" and anything unresolved in "open_questions".
If the request cannot be built from the parts list, still return the closest honest minimal intent and explain the gap in open_questions.

USER REQUEST:
$text

PARTS LIST (authoritative, read from the user's installed KiCad libraries):
$($menu | ConvertTo-Json -Depth 8 -Compress)
"@
    [IO.File]::WriteAllText((Join-Path $runDir 'manus_prompt.txt'), $instructions, $utf8)
    Say "2. Sending to Manus ($Profile profile)..."
    $create = Manus 'POST' 'task.create' @{
        message = @{ content = $instructions }
        interactive_mode = $false
        agent_profile = $Profile
        title = "Copper: $slug"
        structured_output_schema = $schema
    }
    $taskId = $create.task_id
    [IO.File]::WriteAllText((Join-Path $runDir 'manus_task.json'), ($create | ConvertTo-Json -Depth 5), $utf8)
    Say "   task $taskId  $($create.task_url)" 'DarkGray'

    # 3. Grounding gate, with repairs on the same task.
    $seen = @{}
    $verdict = 'REJECTED'; $result = $null; $intentPath = $null
    for ($attempt = 1; $attempt -le $MaxRepairs + 1; $attempt++) {
        $ev = Wait-Result $taskId $seen $runDir
        $sor = $ev.structured_output_result
        if (-not $sor.success) { throw "Manus could not produce the structured result: $($sor.error | ConvertTo-Json -Compress)" }
        $result = if ($sor.value -is [string]) { $sor.value | ConvertFrom-Json } else { $sor.value }
        $intentPath = Join-Path $runDir "intent_attempt$attempt.json"
        [IO.File]::WriteAllText($intentPath, ($result.intent | ConvertTo-Json -Depth 30), $utf8)
        Say "3. Grounding gate, attempt $attempt..."
        $g = Gate $intentPath (Join-Path $runDir "intent_check_attempt$attempt.json")
        $g.text -split "`n" | Where-Object { $_ -and $_ -notmatch '^Nothing was compiled' } | ForEach-Object { Say "   $_" $(if ($g.exit -eq 0) { 'Green' } else { 'Yellow' }) }
        if ($g.exit -eq 0) { $verdict = 'GROUNDED'; break }
        if ($g.exit -ne 2) { throw "The grounding gate could not run (exit $($g.exit))." }
        if ($attempt -gt $MaxRepairs) { break }
        Say '   Sending the findings back to Manus for a correction...' 'Yellow'
        Manus 'POST' 'task.sendMessage' @{
            task_id = $taskId
            message = @{ content = "Copper's grounding gate REJECTED the design intent. Fix every finding below using only the PARTS LIST data, then return the complete corrected structured output:`n$($g.text)" }
            structured_output_schema = $schema
        } | Out-Null
    }
    $taskDetail = (Manus 'GET' "task.detail?task_id=$taskId" $null).task
    $notes = @($result.assumptions | ForEach-Object { "- Assumption: $_" }) + @($result.open_questions | ForEach-Object { "- Open question: $_" })

    if ($verdict -ne 'GROUNDED') {
        Say ''; Say "RESULT: REJECTED. Manus's design still has ungrounded details after $MaxRepairs repair(s); nothing was compiled." 'Red'
        Say "  Findings: $runDir\intent_check_attempt$attempt.json" 'Red'
        if ($notes.Count) { Say "Manus notes (not checked by Copper):"; $notes | ForEach-Object { Say "  $_" } }
        return
    }

    # 4. Compile and check; 5. facts + Manus notes + claim check.
    $final = Join-Path $runDir 'intent.json'
    Copy-Item -LiteralPath $intentPath -Destination $final -Force
    Say '4. Compiling and running KiCad/Copper checks...'
    $copperPs1 = Join-Path $here 'copper.ps1'
    $c = & powershell -NoProfile -ExecutionPolicy Bypass -File $copperPs1 compile -Intent $final -Out $runDir 2>&1 | ForEach-Object { "$_" }
    $cr = ($c | Where-Object { $_ -like 'COPPER_RESULT *' } | Select-Object -Last 1)
    Say "   $cr" 'DarkGray'
    $proj = Join-Path $runDir ([string]$result.intent.project.name)
    if (-not (Test-Path -LiteralPath (Join-Path $proj ("{0}.kicad_pcb" -f $result.intent.project.name)))) {
        Say ''; Say 'RESULT: the compiler rejected the intent after the gate (see compile output above).' 'Red'
        $c | Select-Object -Last 15 | ForEach-Object { Say "  $_" 'DarkGray' }
        return
    }
    & powershell -NoProfile -ExecutionPolicy Bypass -File $copperPs1 report -Project $proj 2>&1 | Out-Null
    $report = Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $proj 'REPORT.md')
    $answer = $report + "`n## Manus notes (not checked by Copper)`n`n" + $(if ($notes.Count) { $notes -join "`n" } else { '- none' }) + "`n"
    [IO.File]::WriteAllText((Join-Path $runDir 'answer.md'), $answer, $utf8)
    $notesFile = Join-Path $runDir 'manus_notes.txt'
    [IO.File]::WriteAllText($notesFile, ($notes -join "`n"), $utf8)
    $cc = & powershell -NoProfile -ExecutionPolicy Bypass -File $copperPs1 claimcheck -Text $notesFile -Project $proj 2>&1 | ForEach-Object { "$_" }

    Say ''; Say '================ RESULT ================' 'Cyan'
    $report -split "`n" | Where-Object { $_ -match '^- ' } | ForEach-Object { Say $_ }
    Say ''; Say "Manus notes (not checked by Copper):"
    if ($notes.Count) { $notes | ForEach-Object { Say "  $_" } } else { Say '  none' }
    $flags = @($cc | Where-Object { $_ -match '^\s+\[' })
    if ($flags.Count) { Say 'Claim check on the Manus notes: statements the facts do not support:' 'Yellow'; $flags | ForEach-Object { Say $_ 'Yellow' } }
    else { Say 'Claim check on the Manus notes: no unsupported claims found.' 'Green' }
    Say ''
    Say "Files:   $proj" 'Gray'
    Say "Answer:  $runDir\answer.md" 'Gray'
    Say ("Manus:   {0}   credits used: {1}" -f $create.task_url, $(if ($null -ne $taskDetail.credit_usage) { $taskDetail.credit_usage } else { 'n/a' })) 'Gray'
}

$text = $Prompt
if ($PromptFile) { $text = Get-Content -Raw -Encoding UTF8 -LiteralPath $PromptFile }
if ($text) { Run-Prompt $text.Trim(); exit 0 }

Say 'Copper prompt console: type a design request and press Enter (empty line to quit).' 'Cyan'
Say "Each request is sent to Manus ($Profile profile) and uses credits on your Manus account." 'DarkGray'
Say 'Tip: name exact parts as "symbol Device:LED footprint LED_SMD:LED_0603_1608Metric" to use them; otherwise standard parts are offered.' 'DarkGray'
while ($true) {
    Write-Host ''
    $line = Read-Host 'Prompt'
    if (-not $line -or -not $line.Trim()) { break }
    try { Run-Prompt $line.Trim() } catch { Say "ERROR: $($_.Exception.Message)" 'Red' }
}
