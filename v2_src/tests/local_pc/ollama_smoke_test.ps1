[CmdletBinding()]
param(
    [string]$Model = 'qwen3:8b',
    [string]$Endpoint = 'http://localhost:11434',
    [string]$OutputRoot = (Join-Path $PSScriptRoot '..\..\..\validation\ollama'),
    [switch]$PullIfMissing
)
$ErrorActionPreference = 'Stop'
$ollama = Get-Command ollama -ErrorAction Stop
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$OutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)

$version = (& $ollama.Source --version 2>&1 | Out-String).Trim()
$listRaw = (& $ollama.Source list 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) { throw "ollama list failed: $listRaw" }
$present = $listRaw -match [regex]::Escape($Model)
if (-not $present -and $PullIfMissing) {
    Write-Host "Pulling $Model..." -ForegroundColor Cyan
    & $ollama.Source pull $Model
    if ($LASTEXITCODE -ne 0) { throw "ollama pull $Model failed" }
    $listRaw = (& $ollama.Source list 2>&1 | Out-String)
    $present = $listRaw -match [regex]::Escape($Model)
}
if (-not $present) { throw "Model '$Model' is not installed. Run: ollama pull $Model" }

$payload = @{
    model = $Model
    stream = $false
    keep_alive = '5m'
    format = 'json'
    messages = @(
        @{ role = 'system'; content = 'You are Copper requirements analyst. Do not invent electrical specifications. Return JSON with explicit_requirements, unknowns, assumptions (must be empty unless explicitly justified), and clarification_questions.' },
        @{ role = 'user'; content = 'Design a small two-layer controller board with an MCU, a status LED and its resistor. The supply voltage, MCU family, LED colour, GPIO current limit and board dimensions are unspecified. Identify known requirements and unknowns. Do not choose parts or invent values.' }
    )
    options = @{ temperature = 0.1; num_ctx = 4096 }
} | ConvertTo-Json -Depth 12

$timer = [System.Diagnostics.Stopwatch]::StartNew()
$response = Invoke-RestMethod -Uri "$Endpoint/api/chat" -Method Post `
    -ContentType 'application/json; charset=utf-8' `
    -Body ([System.Text.Encoding]::UTF8.GetBytes($payload))
$timer.Stop()

$report = [pscustomobject]@{
    model = $Model
    ollama_version = $version
    endpoint = $Endpoint
    elapsed_seconds = [math]::Round($timer.Elapsed.TotalSeconds, 3)
    prompt_tokens = $response.prompt_eval_count
    generated_tokens = $response.eval_count
    response = $response.message.content
    timestamp_utc = [DateTime]::UtcNow.ToString('o')
}
$json = ConvertTo-Json -InputObject $report -Depth 12
$path = Join-Path $OutputRoot 'ollama_smoke.json'
[System.IO.File]::WriteAllText($path, $json, ([System.Text.UTF8Encoding]::new($false)))
Write-Host "Ollama response saved: $path" -ForegroundColor Green
Write-Host "Model: $Model | elapsed: $($report.elapsed_seconds)s | prompt tokens: $($report.prompt_tokens) | generated: $($report.generated_tokens)"
& $ollama.Source ps
if (Get-Command nvidia-smi -ErrorAction SilentlyContinue) {
    nvidia-smi --query-gpu=name,memory.used,memory.total,utilization.gpu --format=csv,noheader
}
