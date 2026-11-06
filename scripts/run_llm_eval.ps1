# Copper LLM eval (track G): run eval/prompts.jsonl through copper-core's agent loop and write
# eval_out/<YYYY-MM-DD>[-N]/results.jsonl and summary.txt (plus per-prompt logs).
#
#   scripts\run_llm_eval.ps1                                   mock backend (deterministic, no model needed)
#   scripts\run_llm_eval.ps1 -Backend ollama -Model qwen3:8b
#   scripts\run_llm_eval.ps1 -Backend ollama -Model llama3.2:latest -Url http://gpu-box:11434
#
# $env:COPPER_CORE overrides the copper-core binary. All the work is in C (copper-core agent-eval); this only picks
# the output folder, which is never reused.
param(
    [ValidateSet('mock', 'ollama')] [string] $Backend = 'mock',
    [string] $Model = '',
    [string] $Url = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$core = $env:COPPER_CORE
if (-not $core) {
    foreach ($c in 'build\copper-core.exe', 'build-local\copper-core.exe', 'build-v2\copper-core.exe', 'build\Release\copper-core.exe') {
        if (Test-Path (Join-Path $root $c)) { $core = Join-Path $root $c; break }
    }
}
if (-not $core) { Write-Error 'copper-core not found: build it or set COPPER_CORE'; exit 64 }
$day = Get-Date -Format 'yyyy-MM-dd'
$out = Join-Path $root "eval_out\$day"
$n = 2
while (Test-Path $out) { $out = Join-Path $root "eval_out\$day-$n"; $n++ }
New-Item -ItemType Directory -Force (Join-Path $root 'eval_out') | Out-Null
$cmdArgs = @('agent-eval', (Join-Path $root 'eval\prompts.jsonl'), $out, '--backend', $Backend, '--root', $root)
if ($Model) { $cmdArgs += @('--model', $Model) }
if ($Url) { $cmdArgs += @('--url', $Url) }
& $core @cmdArgs
$rc = $LASTEXITCODE
Get-Content (Join-Path $out 'summary.txt')
exit $rc
