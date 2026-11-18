[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BoardRoot,

    [string]$CopperExe = (Join-Path $PSScriptRoot '..\..\build\Release\copper.exe'),
    [string]$RulesFile = (Join-Path $PSScriptRoot '..\jlcpcb_rules.rules'),
    [string]$OutputRoot = (Join-Path $PSScriptRoot '..\..\board_test_results'),
    [string]$KiCadCli = 'kicad-cli',

    [ValidateSet('1', '2', '2.5', '3.5', '4.5')]
    [string]$CopperOz = '1',

    [ValidateSet('routed', 'vcut')]
    [string]$Edge = 'routed',

    [ValidateSet('green', 'red', 'yellow', 'blue', 'purple', 'black', 'white')]
    [string]$MaskColor = 'green',

    [switch]$SkipKiCad
)

$ErrorActionPreference = 'Stop'

function Resolve-ExistingFile {
    param([string]$Path, [string]$Description)
    $resolved = Resolve-Path -LiteralPath $Path -ErrorAction Stop
    if (-not (Test-Path -LiteralPath $resolved.Path -PathType Leaf)) {
        throw "$Description is not a file: $Path"
    }
    return $resolved.Path
}

function Invoke-LoggedNative {
    param(
        [string]$Executable,
        [string[]]$Arguments,
        [string]$WorkingDirectory,
        [string]$LogPath
    )

    $output = @()
    $exitCode = 9009
    try {
        Push-Location -LiteralPath $WorkingDirectory
        try {
            $output = @(& $Executable @Arguments 2>&1)
            $exitCode = $LASTEXITCODE
        }
        finally {
            Pop-Location
        }
    }
    catch {
        $output += "COMMAND INVOCATION FAILED: $($_.Exception.Message)"
    }

    $text = ($output | ForEach-Object { [string]$_ }) -join [Environment]::NewLine
    [System.IO.File]::WriteAllText($LogPath, $text, ([System.Text.UTF8Encoding]::new($false)))
    return [pscustomobject]@{
        ExitCode = [int]$exitCode
        LogPath = $LogPath
        Output = $text
    }
}

$boardRootResolved = (Resolve-Path -LiteralPath $BoardRoot -ErrorAction Stop).Path.TrimEnd('\', '/')
$copperPath = Resolve-ExistingFile -Path $CopperExe -Description 'Copper executable'
$rulesPath = Resolve-ExistingFile -Path $RulesFile -Description 'Copper fabrication rules'
$outputBase = [System.IO.Path]::GetFullPath($OutputRoot)
$runId = Get-Date -Format 'yyyyMMdd_HHmmss'
$outputPath = Join-Path $outputBase $runId
New-Item -ItemType Directory -Force -Path $outputPath | Out-Null

$kicadExecutable = $null
if (-not $SkipKiCad) {
    if (Test-Path -LiteralPath $KiCadCli -PathType Leaf) {
        $kicadExecutable = (Resolve-Path -LiteralPath $KiCadCli -ErrorAction Stop).Path
    } else {
        $kicadCommand = Get-Command $KiCadCli -ErrorAction SilentlyContinue
        if ($kicadCommand) {
            $kicadExecutable = $kicadCommand.Source
            if (-not $kicadExecutable) { $kicadExecutable = $kicadCommand.Path }
        }
    }
    if (-not $kicadExecutable) {
        Write-Warning "KiCad CLI '$KiCadCli' was not found. Copper tests will run; KiCad DRC will be recorded as unavailable."
    }
}

$boards = @(Get-ChildItem -LiteralPath $boardRootResolved -Filter '*.kicad_pcb' -File -Recurse | Sort-Object FullName)
if ($boards.Count -eq 0) {
    throw "No .kicad_pcb boards found under $boardRootResolved"
}

$results = New-Object System.Collections.ArrayList
foreach ($board in $boards) {
    $relative = $board.FullName.Substring($boardRootResolved.Length).TrimStart('\', '/')
    $relativeBytes = [System.Text.Encoding]::UTF8.GetBytes($relative)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $relativeHash = ([BitConverter]::ToString($sha.ComputeHash($relativeBytes))).Replace('-', '').Substring(0, 12).ToLowerInvariant()
    }
    finally {
        $sha.Dispose()
    }

    $caseName = ([System.IO.Path]::GetFileNameWithoutExtension($board.Name) -replace '[^A-Za-z0-9._-]', '_') + '_' + $relativeHash
    $caseOutput = Join-Path $outputPath $caseName
    New-Item -ItemType Directory -Force -Path $caseOutput | Out-Null

    $boardHash = (Get-FileHash -LiteralPath $board.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    $rulesHash = (Get-FileHash -LiteralPath $rulesPath -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Host "`n=== $relative ===" -ForegroundColor Cyan

    $inspect = Invoke-LoggedNative -Executable $copperPath `
        -Arguments @('user', 'board-inspect', $board.FullName) `
        -WorkingDirectory $boardRootResolved `
        -LogPath (Join-Path $caseOutput 'copper_inspect.log')

    $validate = Invoke-LoggedNative -Executable $copperPath `
        -Arguments @('dev', 'board', 'validate', $board.FullName) `
        -WorkingDirectory $boardRootResolved `
        -LogPath (Join-Path $caseOutput 'copper_validate.log')

    $dfmReport = Join-Path $caseOutput 'copper_dfm.txt'
    $dfm = Invoke-LoggedNative -Executable $copperPath `
        -Arguments @('user', 'dfm-check', $board.FullName,
                     '--rules', $rulesPath,
                     '--copper-oz', $CopperOz,
                     '--edge', $Edge,
                     '--mask-color', $MaskColor,
                     '--report', $dfmReport) `
        -WorkingDirectory $boardRootResolved `
        -LogPath (Join-Path $caseOutput 'copper_dfm.log')

    $kicadExit = $null
    $kicadReport = $null
    $kicadLog = $null
    if ($kicadExecutable) {
        $kicadReport = Join-Path $caseOutput 'kicad_drc.json'
        $kicad = Invoke-LoggedNative -Executable $kicadExecutable `
            -Arguments @('pcb', 'drc', '--format', 'json', '--severity-all',
                         '--exit-code-violations', '--output', $kicadReport,
                         $board.FullName) `
            -WorkingDirectory $boardRootResolved `
            -LogPath (Join-Path $caseOutput 'kicad_drc.log')
        $kicadExit = $kicad.ExitCode
        $kicadLog = $kicad.LogPath
    }

    $result = [pscustomobject]@{
        Board = $relative
        BoardSha256 = $boardHash
        RulesSha256 = $rulesHash
        InspectExitCode = $inspect.ExitCode
        ValidateExitCode = $validate.ExitCode
        DfmExitCode = $dfm.ExitCode
        DfmReport = if (Test-Path -LiteralPath $dfmReport) { $dfmReport } else { $null }
        KiCadDrcExitCode = $kicadExit
        KiCadDrcReport = if ($kicadReport -and (Test-Path -LiteralPath $kicadReport)) { $kicadReport } else { $null }
        KiCadDrcLog = $kicadLog
        OutputDirectory = $caseOutput
    }
    [void]$results.Add($result)

    Write-Host ("Inspect={0} Validate={1} CopperDFM={2} KiCadDRC={3}" -f `
        $inspect.ExitCode, $validate.ExitCode, $dfm.ExitCode,
        $(if ($null -eq $kicadExit) { 'not-run' } else { $kicadExit }))
}

$summaryJson = Join-Path $outputPath 'board_corpus_summary.json'
$summaryCsv = Join-Path $outputPath 'board_corpus_summary.csv'
$resultArray = [object[]]$results.ToArray()
$json = ConvertTo-Json -InputObject $resultArray -Depth 8
[System.IO.File]::WriteAllText($summaryJson, $json, ([System.Text.UTF8Encoding]::new($false)))
$resultArray | Export-Csv -LiteralPath $summaryCsv -NoTypeInformation -Encoding UTF8

Write-Host "`nCorpus run complete: $($boards.Count) boards" -ForegroundColor Green
Write-Host "Summary JSON: $summaryJson"
Write-Host "Summary CSV : $summaryCsv"
Write-Host 'Copper DFM exit codes: 0=PASS, 2=FAIL, 3=UNVERIFIED; inspect/validate nonzero means input or execution failure.'
Write-Host 'KiCad DRC uses the board project settings. Do not treat it as a rule-equivalent comparison until matching .kicad_dru rules are generated.'
