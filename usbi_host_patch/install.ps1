# Install the accelerated ADIUSBi DLL (keeps the original as ADIUSBi.dll.orig).
#
# Usage (close SigmaStudio first):
#   powershell -ExecutionPolicy Bypass -File install.ps1 -SigmaStudioDir "D:\APPS\SigmaStudio 4.7"

param(
    [string]$SigmaStudioDir = 'D:\APPS\SigmaStudio 4.7'
)

$ErrorActionPreference = 'Stop'

$target = Join-Path $SigmaStudioDir 'ADIUSBi.dll'
$backup = "$target.orig"
$shim   = Join-Path $PSScriptRoot 'dist\ADIUSBi.dll'
$marker = Join-Path $SigmaStudioDir 'ADIUSBi.dll.shim'

if (-not (Test-Path $shim)) {
    throw "Missing $shim - run build.ps1 first"
}
if (-not (Test-Path $target)) {
    throw "Original not found: $target (check -SigmaStudioDir)"
}

$running = Get-Process -Name 'SStudio' -ErrorAction SilentlyContinue
if ($running) {
    throw "SigmaStudio is running (PID $($running.Id)); close it first"
}

if (Test-Path $marker) {
    Write-Host "Accelerated DLL already installed; nothing to do." -ForegroundColor Yellow
    exit 0
}

Copy-Item $target $backup -Force
Copy-Item $shim $target -Force
"ADIUSBi shim installed $(Get-Date -Format o)" | Set-Content -Path $marker -Encoding UTF8

Write-Host "Installed: $target" -ForegroundColor Green
Write-Host "Original backed up as: $backup"
Write-Host "To restore: powershell -ExecutionPolicy Bypass -File uninstall.ps1 -SigmaStudioDir `"$SigmaStudioDir`""
