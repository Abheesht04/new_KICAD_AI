<#
copper.ps1: the one command a Manus agent (Manus Studio, "Work locally") uses to drive Copper on this PC.

Every subcommand runs a fixed Copper, KiCad or compiler command with paths from copper.config.json (written by
manus\setup_manus_workspace.ps1). It prints the tool's own output, then one machine-readable line:

    COPPER_RESULT {"command": "...", "exit": N, "outcome": "...", ...}

and exits with the tool's exit code. The outcome is taken from the tool's exit code and reports; this script
never upgrades a result.

Subcommands:
    doctor                                   what is installed and where; nothing is changed
    context  -Prompt <text> -Out <ctx.json>  real KiCad pin numbers, pin names and pad numbers for the parts a task names
    check    -Intent <intent.json>           grounding gate: every part, pin, pin name, pad and net checked against the
                                             installed KiCad libraries; anything invented is REJECTED (nothing compiled)
    compile  -Intent <intent.json> -Out <dir>  runs the grounding gate first; only a GROUNDED intent is compiled into a
                                             KiCad project, then netlist, ERC and DRC, then Copper import
    report   -Project <dir>                  the facts sheet: REPORT.md and copper_facts.json, built only from tool output
    claimcheck -Text <answer.txt> -Project <dir>  flags statements in a draft answer that the facts contradict
    inspect  -Board <x.kicad_pcb>            board summary (footprints, pads, nets, tracks, unmodelled records)
    dfm      -Board <x.kicad_pcb> [-Profile <json>]  DFM against a cited fab profile (default JLCPCB 2-layer)
    topology -Schematic <x.kicad_sch> -Netlist <x.kicad_net>   pin-to-net table
    checks   -Project <dir>                  summarise the ERC/DRC reports the compile step saved
    workbench                                start the Copper Workbench browser UI (http://127.0.0.1:8765)
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0, Mandatory = $true)]
    [ValidateSet('doctor', 'context', 'check', 'compile', 'inspect', 'dfm', 'topology', 'checks', 'report', 'claimcheck', 'workbench')]
    [string]$Command,
    [string]$Prompt, [string]$Intent, [string]$Out, [string]$Board, [string]$Profile,
    [string]$Schematic, [string]$Netlist, [string]$Project, [string]$Text
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$configPath = Join-Path $here 'copper.config.json'
if (-not (Test-Path -LiteralPath $configPath)) { Write-Error "copper.config.json not found next to copper.ps1; run manus\setup_manus_workspace.ps1"; exit 64 }
$cfg = Get-Content -Raw -Encoding UTF8 -LiteralPath $configPath | ConvertFrom-Json

function Result([string]$cmd, [int]$code, [string]$outcome, [hashtable]$extra) {
    $o = [ordered]@{ command = $cmd; exit = $code; outcome = $outcome }
    if ($extra) { foreach ($k in $extra.Keys) { $o[$k] = $extra[$k] } }
    Write-Output ('COPPER_RESULT ' + ($o | ConvertTo-Json -Compress -Depth 6))
    exit $code
}
function Need([string]$name, [string]$value) { if (-not $value) { Write-Error "$Command needs -$name"; Result $Command 64 'USAGE' @{ missing = $name } } }
function Full([string]$p) { if ([IO.Path]::IsPathRooted($p)) { $p } else { Join-Path (Get-Location) $p } }
function Run([string]$exe, [string[]]$argv) {
    & $exe @argv | Out-Host     # print the tool's output; only the exit code is returned
    return $LASTEXITCODE
}

function Gate([string]$intentPath, [string]$reportPath) {
    & $cfg.copper @('design', 'check-intent', $intentPath, '--symbol-root', $cfg.symbols_root, '--footprint-root', $cfg.footprints_root, '--out', $reportPath) | Out-Host
    return $LASTEXITCODE
}
function KicadSummary([string]$projectDir) {
    $v = Join-Path $projectDir 'validation'
    $summary = [ordered]@{}
    foreach ($name in 'erc', 'drc') {
        $f = Join-Path $v "$name.json"
        if (-not (Test-Path -LiteralPath $f)) { $summary[$name] = 'NOT_RUN'; continue }
        $j = Get-Content -Raw -Encoding UTF8 -LiteralPath $f | ConvertFrom-Json
        $items = @()
        if ($name -eq 'erc') { foreach ($s in @($j.sheets)) { $items += @($s.violations) } }
        else { $items += @($j.violations); $items += @($j.unconnected_items); $items += @($j.schematic_parity) }
        $items = @($items | Where-Object { $_ })
        $bySev = [ordered]@{}
        foreach ($i in $items) { $s = [string]$i.severity; if (-not $bySev.Contains($s)) { $bySev[$s] = 0 }; $bySev[$s]++ }
        $types = @($items | ForEach-Object { $_.type } | Group-Object | Sort-Object Count -Descending | ForEach-Object { "$($_.Name) x$($_.Count)" })
        $summary[$name] = [ordered]@{ findings = $items.Count; by_severity = $bySev; types = $types; kicad_version = $j.kicad_version }
    }
    return $summary
}

switch ($Command) {
    'doctor' {
        $items = [ordered]@{}
        foreach ($k in 'copper', 'copper_core', 'python', 'kicad_cli', 'symbols_root', 'footprints_root', 'compiler', 'asset_context', 'intent_schema', 'fab_profile', 'repo') {
            $v = $cfg.$k
            $items[$k] = [ordered]@{ path = $v; present = [bool]($v -and (Test-Path -LiteralPath $v)) }
        }
        $kv = ''
        if ($items.kicad_cli.present) { $kv = (& $cfg.kicad_cli version 2>$null | Select-Object -First 1) }
        $items.kicad_cli['version'] = $kv
        $items.kicad_cli['kicad9'] = [bool]($kv -match '^9\.')
        $items | ConvertTo-Json -Depth 4
        $missing = @($items.Keys | Where-Object { -not $items[$_].present })
        if ($missing.Count) { Result 'doctor' 3 'UNVERIFIED' @{ missing = $missing; kicad_version = $kv } }
        $note = if ($kv -match '^9\.') { 'KiCad 9.x: native evidence' } else { 'KiCad is not 9.x: its ERC/DRC results are a development check, not native evidence' }
        Result 'doctor' 0 'READY' @{ kicad_version = $kv; note = $note }
    }
    'context' {
        Need 'Prompt' $Prompt; Need 'Out' $Out
        $tmp = Join-Path $env:TEMP ('copper_prompt_' + [guid]::NewGuid().ToString('N') + '.txt')
        [IO.File]::WriteAllText($tmp, $Prompt, (New-Object Text.UTF8Encoding($false)))
        try { $code = Run $cfg.python @($cfg.asset_context, '--prompt-file', $tmp, '--symbols-root', $cfg.symbols_root, '--footprints-root', $cfg.footprints_root, '--out', (Full $Out)) }
        finally { Remove-Item -LiteralPath $tmp -ErrorAction SilentlyContinue }
        $outcome = if ($code -eq 0) { 'OK' } else { 'ERROR' }
        Result 'context' $code $outcome @{ out = (Full $Out) }
    }
    'check' {
        Need 'Intent' $Intent
        $rp = Join-Path (Split-Path -Parent (Full $Intent)) 'intent_check.json'
        $code = Gate (Full $Intent) $rp
        $outcome = switch ($code) { 0 { 'GROUNDED' } 2 { 'REJECTED' } 64 { 'USAGE' } default { 'ERROR' } }
        Result 'check' $code $outcome @{ report = $rp; next = $(if ($code -eq 0) { 'compile' } else { 'fix every finding from the context data, then check again; do not compile' }) }
    }
    'compile' {
        Need 'Intent' $Intent; Need 'Out' $Out
        $outDir = Full $Out
        New-Item -ItemType Directory -Force -Path $outDir | Out-Null
        $gateReport = Join-Path $outDir 'intent_check.json'
        $g = Gate (Full $Intent) $gateReport
        if ($g -ne 0) { Result 'compile' 2 'REJECTED_UNGROUNDED' @{ out = $outDir; gate_report = $gateReport; note = 'the grounding gate rejected the intent; nothing was compiled' } }
        $code = Run $cfg.python @($cfg.compiler, '--intent', (Full $Intent), '--out', $outDir, '--symbols-root', $cfg.symbols_root, '--footprints-root', $cfg.footprints_root, '--kicad-cli', $cfg.kicad_cli, '--copper-cli', $cfg.copper)
        $meaning = switch ($code) { 0 { 'GENERATED_NOT_ROUTED' } 2 { 'INVALID_INPUT' } 3 { 'ERROR_NETLIST_EXPORT' } default { 'ERROR_COPPER_IMPORT' } }
        Result 'compile' $code $meaning @{ out = $outDir; next = 'run: .\copper.ps1 checks -Project <out>\<project.name>  (exit 0 here does NOT mean ERC/DRC are clean)' }
    }
    'inspect' {
        Need 'Board' $Board
        $code = Run $cfg.copper @('user', 'board-inspect', (Full $Board))
        $outcome = if ($code -eq 0) { 'OK' } else { 'ERROR' }
        Result 'inspect' $code $outcome @{ board = (Full $Board) }
    }
    'dfm' {
        Need 'Board' $Board
        $prof = if ($Profile) { Full $Profile } else { $cfg.fab_profile }
        $code = Run $cfg.copper_core @('dfm', (Full $Board), $prof)
        $outcome = switch ($code) { 0 { 'PASS_OR_UNVERIFIED_SEE_JSON' } 2 { 'FAIL' } 1 { 'INVALID_INPUT' } default { 'ERROR' } }
        Result 'dfm' $code $outcome @{ board = (Full $Board); profile = $prof; read = 'verdict and rules[] in the JSON above; UNVERIFIED rules are not passes' }
    }
    'topology' {
        Need 'Schematic' $Schematic; Need 'Netlist' $Netlist
        $code = Run $cfg.copper @('user', 'topology', 'show', (Full $Schematic), '--netlist', (Full $Netlist))
        $outcome = switch ($code) { 0 { 'COMPLETE' } 2 { 'INCOMPLETE' } default { 'ERROR' } }
        Result 'topology' $code $outcome @{}
    }
    'checks' {
        Need 'Project' $Project
        $summary = KicadSummary (Full $Project)
        $summary | ConvertTo-Json -Depth 5
        $errs = 0
        foreach ($name in 'erc', 'drc') { if ($summary[$name] -is [System.Collections.IDictionary] -and $summary[$name].by_severity.Contains('error')) { $errs += $summary[$name].by_severity['error'] } }
        $notRun = @('erc', 'drc' | Where-Object { $summary[$_] -eq 'NOT_RUN' })
        if ($notRun.Count) { Result 'checks' 3 'UNVERIFIED' @{ not_run = $notRun } }
        if ($errs) { Result 'checks' 1 'FAIL' @{ errors = $errs } }
        Result 'checks' 0 'NO_ERRORS_SEE_WARNINGS' @{ note = 'warnings are listed above; an unrouted board is never manufacturing-ready' }
    }
    'report' {
        Need 'Project' $Project
        $proj = Full $Project
        $pcb = Get-ChildItem -LiteralPath $proj -Filter '*.kicad_pcb' | Select-Object -First 1
        if (-not $pcb) { Result 'report' 1 'ERROR' @{ note = "no .kicad_pcb in $proj" } }
        $gatePath = Join-Path (Split-Path -Parent $proj) 'intent_check.json'
        $gate = if (Test-Path -LiteralPath $gatePath) { (Get-Content -Raw -Encoding UTF8 -LiteralPath $gatePath | ConvertFrom-Json).verdict } else { 'NOT_RUN' }
        $insp = (& $cfg.copper @('user', 'board-inspect', $pcb.FullName) 2>&1 | ForEach-Object { "$_" }) -join "`n"
        $m = [regex]::Match($insp, 'Tracks:\s*(\d+)\s+Arc tracks:\s*(\d+)\s+Vias:\s*(\d+)')
        $tracks = if ($m.Success) { [int]$m.Groups[1].Value + [int]$m.Groups[2].Value } else { -1 }
        $vias = if ($m.Success) { [int]$m.Groups[3].Value } else { -1 }
        $fp = [regex]::Match($insp, 'Footprints:\s*(\d+)')
        $dfmJson = (& $cfg.copper_core @('dfm', $pcb.FullName, $cfg.fab_profile) 2>$null | Out-String)
        $dfm = $null; try { $dfm = $dfmJson | ConvertFrom-Json } catch { }
        $kicad = KicadSummary $proj
        $facts = [ordered]@{
            format = 'copper.facts/1'; project = $proj; board = $pcb.Name
            grounding_gate = $gate
            footprints = $(if ($fp.Success) { [int]$fp.Groups[1].Value } else { -1 })
            tracks = $tracks; vias = $vias; routed = ($tracks -gt 0)
            erc = $kicad['erc']; drc = $kicad['drc']
            dfm_verdict = $(if ($dfm) { $dfm.verdict } else { 'NOT_RUN' })
            dfm_not_passed = @($(if ($dfm) { $dfm.rules | Where-Object { $_.verdict -ne 'PASS' } | ForEach-Object { "$($_.rule): $($_.verdict)" } }))
            dfm_profile = $(if ($dfm) { "$($dfm.profile.name) ($($dfm.profile.source), retrieved $($dfm.profile.retrieved))" } else { '' })
            manufacturing = 'NOT APPROVED: a generated draft; human engineering review is required'
            kicad_note = $(if ("$($kicad['drc'].kicad_version)" -match '^9\.') { 'KiCad 9.x native checks' } else { 'KiCad is not 9.x: ERC/DRC are a development check, not native evidence' })
        }
        [IO.File]::WriteAllText((Join-Path $proj 'copper_facts.json'), ($facts | ConvertTo-Json -Depth 6), (New-Object Text.UTF8Encoding($false)))
        $erc = $facts.erc; $drc = $facts.drc
        $lines = @(
            "# Copper facts: $($pcb.BaseName)", '',
            'Generated by copper.ps1 report from tool output only. Paste it unchanged; do not add verdicts.', '',
            "- Grounding gate (parts, pins, pin names, pads, nets vs installed KiCad libraries): **$gate**",
            "- Footprints on the board: $($facts.footprints)",
            "- Routing: **$(if ($facts.routed) { "$tracks track segment(s), $vias via(s)" } else { 'NOT ROUTED (0 tracks)' })**",
            "- ERC: $(if ($erc -is [System.Collections.IDictionary]) { "$($erc.findings) finding(s) $(($erc.by_severity.GetEnumerator() | ForEach-Object { "$($_.Key) $($_.Value)" }) -join ', '): $($erc.types -join ', ')" } else { $erc })",
            "- DRC: $(if ($drc -is [System.Collections.IDictionary]) { "$($drc.findings) finding(s) $(($drc.by_severity.GetEnumerator() | ForEach-Object { "$($_.Key) $($_.Value)" }) -join ', '): $($drc.types -join ', ')" } else { $drc })",
            "- DFM: **$($facts.dfm_verdict)** against $($facts.dfm_profile)$(if ($facts.dfm_not_passed.Count) { '; not passed: ' + ($facts.dfm_not_passed -join '; ') } else { '' })",
            "- $($facts.kicad_note)",
            "- Manufacturing: **$($facts.manufacturing)**"
        )
        [IO.File]::WriteAllText((Join-Path $proj 'REPORT.md'), ($lines -join "`n") + "`n", (New-Object Text.UTF8Encoding($false)))
        $lines | ForEach-Object { Write-Output $_ }
        Result 'report' 0 'FACTS_WRITTEN' @{ report = (Join-Path $proj 'REPORT.md'); facts = (Join-Path $proj 'copper_facts.json') }
    }
    'claimcheck' {
        Need 'Text' $Text; Need 'Project' $Project
        $proj = Full $Project
        $factsPath = Join-Path $proj 'copper_facts.json'
        if (-not (Test-Path -LiteralPath $factsPath)) { Result 'claimcheck' 3 'UNVERIFIED' @{ note = 'run: .\copper.ps1 report -Project <dir> first' } }
        $f = Get-Content -Raw -Encoding UTF8 -LiteralPath $factsPath | ConvertFrom-Json
        $answer = Get-Content -Raw -Encoding UTF8 -LiteralPath (Full $Text)
        $issues = New-Object System.Collections.Generic.List[object]
        $ercFindings = if ($f.erc -is [string]) { -1 } else { [int]$f.erc.findings }
        $drcFindings = if ($f.drc -is [string]) { -1 } else { [int]$f.drc.findings }
        # Each rule: a claim pattern, and when the facts contradict it (or it is never allowed).
        $rules = @(
            @{ id = 'ROUTED'; re = '\b(fully\s+|completely\s+)?routed\b|\btraces?\s+(are|were)\s+(laid|added|routed)'; bad = (-not $f.routed); why = "the board has $($f.tracks) track segment(s): it is not routed" },
            @{ id = 'ERC_CLEAN'; re = '\bERC\b[^.\n]{0,40}\b(pass(ed|es)?|clean|no\s+(errors|violations|issues|findings)|0\s+(errors|violations))'; bad = ($ercFindings -ne 0); why = "ERC lists $ercFindings finding(s)" },
            @{ id = 'DRC_CLEAN'; re = '\bDRC\b[^.\n]{0,40}\b(pass(ed|es)?|clean|no\s+(errors|violations|issues|findings)|0\s+(errors|violations))'; bad = ($drcFindings -ne 0); why = "DRC lists $drcFindings finding(s)" },
            @{ id = 'DFM_PASS'; re = '\bDFM\b[^.\n]{0,40}\b(pass(ed|es)?|clean|no\s+(issues|problems|findings))'; bad = ($f.dfm_verdict -ne 'PASS'); why = "the DFM verdict is $($f.dfm_verdict)" },
            @{ id = 'MANUFACTURING_READY'; re = '(manufactur(ing|e)[\s-]*ready|ready\s+(to|for)\s+(manufactur\w*|fabricat\w*|order\w*|production|fab)|production[\s-]*ready|manufacturable|approved\s+for\s+(manufactur|production|fabricat)|good\s+to\s+go)'; bad = $true; why = 'every Copper design is NOT APPROVED for manufacturing; human review is required' },
            @{ id = 'PART_DATA'; re = '\b(MPN|manufacturer\s+part\s+number|LCSC\s*(part|#|number)?|in\s+stock|datasheet\s+(rating|value)s?)\b'; bad = $true; why = 'Copper holds no MPN, stock or rating data for this design; such statements are unsupported' },
            @{ id = 'VERIFIED'; re = '\b(fully\s+)?(verified|validated|guaranteed)\b'; bad = $true; why = 'Copper reports findings, not verification; use the facts sheet wording' }
        )
        $negation = '(\bnot\b|n''t\b|\bnever\b|\bno\b|\bwithout\b|\bunrouted\b|\bisn''t\b|\bnor\b)'
        foreach ($rule in $rules) {
            foreach ($m in [regex]::Matches($answer, $rule.re, 'IgnoreCase')) {
                # A negation counts only within the three words right before the claim ("is not routed",
                # "never verified"); a "no" elsewhere in the sentence ("no errors and it is ready...") does not.
                $start = [Math]::Max(0, $m.Index - 40)
                $before = $answer.Substring($start, $m.Index - $start)
                $clauseStart = $before.LastIndexOfAny([char[]]'.!?:;,' + [char]10)
                if ($clauseStart -ge 0) { $before = $before.Substring($clauseStart + 1) }
                $words = @($before -split '\s+' | Where-Object { $_ })
                $near = ($words | Select-Object -Last 3) -join ' '
                if ($near -match $negation) { continue }
                if ($rule.bad) { $issues.Add([ordered]@{ claim = $rule.id; text = $m.Value.Trim(); why = $rule.why }) }
            }
        }
        if ($issues.Count) {
            Write-Output 'Statements the facts do not support (rewrite them from REPORT.md):'
            foreach ($i in $issues) { Write-Output ("  [{0}] ""{1}"": {2}" -f $i.claim, $i.text, $i.why) }
            Result 'claimcheck' 1 'CONTRADICTED' @{ issues = $issues.Count }
        }
        Result 'claimcheck' 0 'NO_UNSUPPORTED_CLAIMS_FOUND' @{ note = 'a keyword check, not proof of correctness: REPORT.md stays the authority' }
    }
    'workbench' {
        $root = $cfg.repo
        Start-Process -FilePath $cfg.copper_core -WorkingDirectory $root -WindowStyle Minimized -ArgumentList @('serve-http', '--port', '8765', '--root', "`"$root`"", '--kicad-cli', "`"$($cfg.kicad_cli)`"", '--ui', "`"$(Join-Path $root 'apps\web\index.html')`"")
        Result 'workbench' 0 'STARTED' @{ url = 'http://127.0.0.1:8765/' }
    }
}
