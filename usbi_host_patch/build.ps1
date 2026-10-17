# Build the ADIUSBi-compatible DLL (ADIUSBi.dll).
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File build.ps1 -SigmaStudioDir "D:\APPS\SigmaStudio 4.7"
#
# Output: dist\ADIUSBi.dll

param(
    [string]$SigmaStudioDir = 'D:\APPS\SigmaStudio 4.7'
)

$ErrorActionPreference = 'Stop'

$cyusb = Join-Path $SigmaStudioDir 'CyUSB.dll'
if (-not (Test-Path $cyusb)) {
    throw "CyUSB.dll not found: $cyusb (use -SigmaStudioDir to point at your SigmaStudio folder)"
}

$csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework\v4.0.30319\csc.exe'
if (-not (Test-Path $csc)) {
    throw "csc.exe not found: $csc"
}

$dist = Join-Path $PSScriptRoot 'dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$out = Join-Path $dist 'ADIUSBi.dll'

& $csc /nologo /target:library /platform:x86 /optimize+ /warn:4 /codepage:65001 `
    /r:"$cyusb" `
    /out:"$out" `
    (Join-Path $PSScriptRoot 'AssemblyInfo.cs') `
    (Join-Path $PSScriptRoot 'ADIUSBiShim.cs')

if ($LASTEXITCODE -ne 0) {
    throw "build failed (csc exit $LASTEXITCODE)"
}

Write-Host "Built: $out" -ForegroundColor Green
Write-Host ("Size: {0:N0} bytes" -f (Get-Item $out).Length)
