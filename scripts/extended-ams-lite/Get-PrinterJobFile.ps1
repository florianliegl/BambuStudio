<#
.SYNOPSIS
  Read-only download of one Extended AMS Lite job file from the printer's FTPS storage.

.DESCRIPTION
  Studio uploads each prepared job as "EAMS-<job id>.gcode.3mf" (the name is shown in the
  Studio log as project_name). This script lists the printer's storage root and /cache,
  prints only the names and sizes of files whose name contains "EAMS-<job id>", and downloads
  those files into -OutDir. It never uploads, renames, deletes or sends printer commands.

  The printer address and access code are typed at the prompts. They are not accepted as
  parameters, not written to disk, and not echoed. The access code is passed to curl through
  stdin so it does not appear in the process list.

  Requires curl.exe (included with Windows 10/11). The printer uses a self-signed certificate,
  so certificate verification is skipped for this LAN-only connection.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\extended-ams-lite\Get-PrinterJobFile.ps1
#>
[CmdletBinding()]
param(
    [ValidatePattern('^[0-9a-f]{16}$')]
    [string]$JobId = '321fecff58bc4efb',

    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
if (-not $OutDir) {
    $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
    $OutDir = Join-Path $scriptDir '..\..\build\extended-ams-failed-slice'
}

$curl = Get-Command curl.exe -ErrorAction SilentlyContinue
if (-not $curl) { throw 'curl.exe was not found.' }

$address = (Read-Host 'Printer IP address').Trim()
if ($address -notmatch '^[0-9A-Za-z.\-]+$') { throw 'That does not look like an address.' }
$secure = Read-Host 'Printer access code (input is hidden)' -AsSecureString
$bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
try {
    $code = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr)
} finally {
    [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr)
}
if ($code -notmatch '^[0-9A-Za-z]+$') { throw 'The access code should contain only letters and digits.' }

$config = "user = `"bblp:$code`""
$needle = "EAMS-$JobId"

function Invoke-Curl([string[]]$Arguments) {
    $output = $config | & $curl.Source -sS -k -K - --connect-timeout 10 --max-time 600 @Arguments
    if ($LASTEXITCODE -ne 0) { throw "curl failed with exit code $LASTEXITCODE." }
    $output
}

$null = New-Item -ItemType Directory -Force -Path $OutDir
$found = @()
foreach ($folder in '/', '/cache/') {
    Write-Host "Listing $folder ..."
    $names = @(Invoke-Curl @('--list-only', "ftps://${address}:990$folder")) |
        ForEach-Object { "$_".Trim() } | Where-Object { $_ -like "*$needle*" }
    foreach ($name in $names) { $found += [pscustomobject]@{ Folder = $folder; Name = $name } }
}

if (-not $found) {
    Write-Host "No file containing '$needle' was found in / or /cache/."
    $code = $null
    return
}

foreach ($item in $found) {
    $target = Join-Path $OutDir $item.Name
    Write-Host "Downloading $($item.Folder)$($item.Name) ..."
    $null = Invoke-Curl @('-o', $target, "ftps://${address}:990$($item.Folder)$([uri]::EscapeDataString($item.Name))")
    $size = (Get-Item $target).Length
    Write-Host ("  saved {0} ({1:N0} bytes)" -f $target, $size)
}
$code = $null
Write-Host 'Done. Nothing was changed on the printer.'
