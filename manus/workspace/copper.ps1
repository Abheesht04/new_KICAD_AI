<#
copper.ps1: the one command a Manus agent (Manus Studio, "Work locally") uses to drive Copper on this PC.

Every subcommand runs a fixed Copper, KiCad or compiler command with paths from copper.config.json (written by
manus\setup_manus_workspace.ps1). It prints the tool's own output, then one machine-readable line:

    COPPER_RESULT {"command": "...", "exit": N, "outcome": "...", ...}

and exits with the tool's exit code. The outcome is taken from the tool's exit code and reports; this script
never upgrades a result.

Subcommands:
    doctor                                   what is installed and where; nothing is changed
    context  -Prompt <text> -Out <ctx.json>  real KiCad pin and pad numbers for the parts a task mentions
    compile  -Intent <intent.json> -Out <dir>  design-intent JSON -> KiCad project, then netlist, ERC and DRC, then Copper import
    inspect  -Board <x.kicad_pcb>            board summary (footprints, pads, nets, tracks, unmodelled records)
    dfm      -Board <x.kicad_pcb> [-Profile <json>]  DFM against a cited fab profile (default JLCPCB 2-layer)
    topology -Schematic <x.kicad_sch> -Netlist <x.kicad_net>   pin-to-net table
    checks   -Project <dir>                  summarise the ERC/DRC reports the compile step saved
    workbench                                start the Copper Workbench browser UI (http://127.0.0.1:8765)
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0, Mandatory = $true)]
    [ValidateSet('doctor', 'context', 'compile', 'inspect', 'dfm', 'topology', 'checks', 'workbench')]
    [string]$Command,
    [string]$Prompt, [string]$Intent, [string]$Out, [string]$Board, [string]$Profile,
    [string]$Schematic, [string]$Netlist, [string]$Project
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
    'compile' {
        Need 'Intent' $Intent; Need 'Out' $Out
        $outDir = Full $Out
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
        $v = Join-Path (Full $Project) 'validation'
        $summary = [ordered]@{}
        foreach ($name in 'erc', 'drc') {
            $f = Join-Path $v "$name.json"
            if (-not (Test-Path -LiteralPath $f)) { $summary[$name] = 'NOT_RUN'; continue }
            $j = Get-Content -Raw -Encoding UTF8 -LiteralPath $f | ConvertFrom-Json
            $items = @()
            if ($name -eq 'erc') { foreach ($s in @($j.sheets)) { $items += @($s.violations) } }
            else { $items += @($j.violations); $items += @($j.unconnected_items); $items += @($j.schematic_parity) }
            $items = @($items | Where-Object { $_ })
            $bySev = @{}
            foreach ($i in $items) { $s = [string]$i.severity; if (-not $bySev.ContainsKey($s)) { $bySev[$s] = 0 }; $bySev[$s]++ }
            $types = @($items | ForEach-Object { $_.type } | Group-Object | Sort-Object Count -Descending | ForEach-Object { "$($_.Name) x$($_.Count)" })
            $summary[$name] = [ordered]@{ findings = $items.Count; by_severity = $bySev; types = $types; kicad_version = $j.kicad_version }
        }
        $summary | ConvertTo-Json -Depth 5
        $errs = 0
        foreach ($name in 'erc', 'drc') { if ($summary[$name] -is [System.Collections.IDictionary] -and $summary[$name].by_severity.ContainsKey('error')) { $errs += $summary[$name].by_severity['error'] } }
        $notRun = @('erc', 'drc' | Where-Object { $summary[$_] -eq 'NOT_RUN' })
        if ($notRun.Count) { Result 'checks' 3 'UNVERIFIED' @{ not_run = $notRun } }
        if ($errs) { Result 'checks' 1 'FAIL' @{ errors = $errs } }
        Result 'checks' 0 'NO_ERRORS_SEE_WARNINGS' @{ note = 'warnings are listed above; an unrouted board is never manufacturing-ready' }
    }
    'workbench' {
        $root = $cfg.repo
        Start-Process -FilePath $cfg.copper_core -WorkingDirectory $root -WindowStyle Minimized -ArgumentList @('serve-http', '--port', '8765', '--root', "`"$root`"", '--kicad-cli', "`"$($cfg.kicad_cli)`"", '--ui', "`"$(Join-Path $root 'apps\web\index.html')`"")
        Result 'workbench' 0 'STARTED' @{ url = 'http://127.0.0.1:8765/' }
    }
}
