# Restore the original ADIUSBi.dll.
#
# Usage (close SigmaStudio first):
#   powershell -ExecutionPolicy Bypass -File uninstall.ps1 -SigmaStudioDir "D:\APPS\SigmaStudio 4.7"

param(
    [string]$SigmaStudioDir = 'D:\APPS\SigmaStudio 4.7'
)

$ErrorActionPreference = 'Stop'

$target = Join-Path $SigmaStudioDir 'ADIUSBi.dll'
$backup = "$target.orig"
$marker = Join-Path $SigmaStudioDir 'ADIUSBi.dll.shim'

$running = Get-Process -Name 'SStudio' -ErrorAction SilentlyContinue
if ($running) {
    throw "SigmaStudio is running (PID $($running.Id)); close it first"
}

if (-not (Test-Path $backup)) {
    Write-Host "No backup found ($backup); accelerated DLL was probably never installed." -ForegroundColor Yellow
    exit 0
}

Copy-Item $backup $target -Force
Remove-Item $backup -Force
if (Test-Path $marker) { Remove-Item $marker -Force }

Write-Host "Restored original: $target" -ForegroundColor Green
