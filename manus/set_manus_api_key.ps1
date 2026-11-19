<#
Store, check or remove your Manus API key on this PC, without it ever appearing on screen, in a file in the
repository, or in a chat.

    powershell -ExecutionPolicy Bypass -File .\manus\set_manus_api_key.ps1            # store (asks for the key)
    powershell -ExecutionPolicy Bypass -File .\manus\set_manus_api_key.ps1 -Status    # is a key stored?
    powershell -ExecutionPolicy Bypass -File .\manus\set_manus_api_key.ps1 -Remove    # delete it

Storage: %APPDATA%\Copper\manus_api_key.dpapi, encrypted with Windows DPAPI for your Windows account only (another
user, or the file copied to another PC, cannot decrypt it). It is outside the repository, so it can never be
committed. Copper code that needs the key reads it with Get-ManusApiKey (below) and sends it only to the Manus API
(https://api.manus.ai, header x-manus-api-key).

The Manus Studio setup (manus\README.md) does not need this key; it signs in with your Manus account.
#>
[CmdletBinding()]
param([switch]$Status, [switch]$Remove)
$ErrorActionPreference = 'Stop'
$dir = Join-Path $env:APPDATA 'Copper'
$file = Join-Path $dir 'manus_api_key.dpapi'

function Get-ManusApiKey {
    <# Returns the stored key as plain text for an API call, or $null. Never print what it returns. #>
    if (-not (Test-Path -LiteralPath $file)) { return $null }
    $secure = (Get-Content -Raw -LiteralPath $file).Trim() | ConvertTo-SecureString
    $ptr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try { return [Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr) }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr) }
}

if ($Remove) {
    if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file -Force; Write-Host 'Manus API key removed.' -ForegroundColor Yellow }
    else { Write-Host 'No Manus API key was stored.' }
    exit 0
}

if ($Status) {
    if (-not (Test-Path -LiteralPath $file)) { Write-Host 'No Manus API key is stored.' -ForegroundColor Yellow; exit 3 }
    try {
        $k = Get-ManusApiKey
        $when = (Get-Item -LiteralPath $file).LastWriteTime.ToString('yyyy-MM-dd HH:mm')
        Write-Host ("A Manus API key is stored ({0} characters, saved {1}) and decrypts for this Windows user." -f $k.Length, $when) -ForegroundColor Green
        $k = $null
        exit 0
    } catch {
        Write-Host 'A key file exists but cannot be decrypted by this Windows user. Store the key again.' -ForegroundColor Red
        exit 1
    }
}

Write-Host 'Paste your Manus API key and press Enter. Nothing is shown while you type or paste.' -ForegroundColor Cyan
Write-Host '(Get or create a key in your Manus account settings. Close this window to cancel.)' -ForegroundColor DarkGray
$secure = Read-Host -AsSecureString 'Manus API key'
$ptr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
try { $plain = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr) } finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr) }
$plain = $plain.Trim()
if (-not $plain) { Write-Host 'Nothing was entered; no key stored.' -ForegroundColor Yellow; exit 2 }
if ($plain -match '\s') { $plain = $null; Write-Host 'The key contains spaces or line breaks; it was not stored. Copy it again exactly.' -ForegroundColor Red; exit 2 }
if ($plain.Length -lt 16) { $len = $plain.Length; $plain = $null; Write-Host "That is only $len characters, which is too short for an API key; it was not stored." -ForegroundColor Red; exit 2 }

New-Item -ItemType Directory -Force -Path $dir | Out-Null
$enc = ConvertTo-SecureString -String $plain -AsPlainText -Force | ConvertFrom-SecureString   # DPAPI, this user only
$length = $plain.Length
$plain = $null
[IO.File]::WriteAllText($file, $enc, (New-Object Text.UTF8Encoding($false)))
Write-Host ("Manus API key stored ({0} characters), encrypted for this Windows user:" -f $length) -ForegroundColor Green
Write-Host "  $file"
Write-Host 'Check it any time with -Status, delete it with -Remove.' -ForegroundColor DarkGray
