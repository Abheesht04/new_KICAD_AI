$ErrorActionPreference = 'Stop'

$endpoint = 'http://localhost:11434/api/chat'
$models = @(
    'qwen3:8b',
    'llama3.2:latest'
)

$task = @'
A user asks Copper to help design a small, two-layer sensor board:

"I need a board powered from USB-C. It should provide a regulated 3.3 V rail and connect a temperature sensor to an MCU over I2C. I have not selected the MCU or sensor yet. I do not know whether USB Power Delivery is required. Keep the board compact."

Respond in plain English. Do not create a circuit or choose part numbers yet.
1. Separate explicit requirements from assumptions.
2. Identify design decisions that could materially change the circuit or component selection.
3. Ask at most three high-value clarification questions.
4. State which parts of the design can be investigated without those answers.

Do not invent part numbers, current requirements, battery requirements, USB-PD support, or physical dimensions. Do not pretend unknown information is specified.
'@

foreach ($model in $models) {
    Write-Host "`n============================================================" -ForegroundColor Cyan
    Write-Host "MODEL: $model" -ForegroundColor Cyan
    Write-Host "============================================================" -ForegroundColor Cyan

    $payload = @{
        model = $model
        messages = @(
            @{
                role = 'system'
                content = 'You are Copper AI, an electronics design assistant. Understand ordinary language, distinguish facts from assumptions, and be explicit about uncertainty. Do not fabricate component specifications.'
            },
            @{
                role = 'user'
                content = $task
            }
        )
        stream = $false
        keep_alive = '5m'
        options = @{
            temperature = 0.2
            num_ctx = 4096
        }
    } | ConvertTo-Json -Depth 10

    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        $response = Invoke-RestMethod `
            -Uri $endpoint `
            -Method Post `
            -ContentType 'application/json; charset=utf-8' `
            -Body ([System.Text.Encoding]::UTF8.GetBytes($payload))
    }
    catch {
        Write-Host "REQUEST FAILED: $($_.Exception.Message)" -ForegroundColor Red
        Write-Host 'Check that the Ollama service is running at localhost:11434.'
        continue
    }
    finally {
        $timer.Stop()
    }

    Write-Host "`n--- MODEL RESPONSE ---" -ForegroundColor Yellow
    Write-Output $response.message.content

    $tokensPerSecond = 0.0
    if ($response.eval_duration -gt 0) {
        $tokensPerSecond = [math]::Round(
            ($response.eval_count / ($response.eval_duration / 1000000000.0)), 2
        )
    }
    $loadSeconds = [math]::Round(($response.load_duration / 1000000000.0), 2)

    Write-Host "`n--- METRICS ---" -ForegroundColor Yellow
    Write-Host "Elapsed wall time : $([math]::Round($timer.Elapsed.TotalSeconds, 2)) s"
    Write-Host "Model load time   : $loadSeconds s"
    Write-Host "Prompt tokens     : $($response.prompt_eval_count)"
    Write-Host "Generated tokens  : $($response.eval_count)"
    Write-Host "Generation speed  : $tokensPerSecond tokens/s"

    Write-Host "`n--- OLLAMA PROCESSOR PLACEMENT ---" -ForegroundColor Yellow
    & ollama ps

    Write-Host "`n--- GPU MEMORY ---" -ForegroundColor Yellow
    & nvidia-smi --query-gpu=name,memory.used,memory.total,utilization.gpu --format=csv,noheader
}
