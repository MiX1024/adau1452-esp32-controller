# ============================================================
#  编译 DspVolumeCtl.exe
#  只用 Windows 自带的 .NET Framework 编译器（csc.exe），
#  无需安装 Visual Studio / .NET SDK，产物为单个绿色 exe。
#
#  用法：  powershell -ExecutionPolicy Bypass -File build.ps1
# ============================================================
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$src  = Join-Path $root "src"
$out  = Join-Path $root "dist"
New-Item -ItemType Directory -Force -Path $out | Out-Null

# ---- 定位 csc.exe（Win11 自带 .NET Framework 4.8 就有） ----
$csc = Join-Path $env:WINDIR "Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if (-not (Test-Path $csc)) {
    $csc = Join-Path $env:WINDIR "Microsoft.NET\Framework\v4.0.30319\csc.exe"
}
if (-not (Test-Path $csc)) {
    throw "找不到 csc.exe，请确认已安装 .NET Framework 4.x"
}

# ---- 生成 exe 图标（app.ico，已存在则跳过） ----
$ico = Join-Path $out "app.ico"
if (-not (Test-Path $ico)) {
    Add-Type -AssemblyName System.Drawing
    $bmp = New-Object System.Drawing.Bitmap 64, 64
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.Clear([System.Drawing.Color]::Transparent)

    $bg = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 38, 38, 48))
    $g.FillEllipse($bg, 2, 2, 60, 60)

    $fg = [System.Drawing.Color]::FromArgb(235, 235, 242)
    $white = New-Object System.Drawing.SolidBrush ($fg)
    $pen = New-Object System.Drawing.Pen ($fg), 5
    $pen.StartCap = 'Round'
    $pen.EndCap = 'Round'

    $pts = @(
        (New-Object System.Drawing.PointF -ArgumentList 3, 24),
        (New-Object System.Drawing.PointF -ArgumentList 15, 24),
        (New-Object System.Drawing.PointF -ArgumentList 27, 11),
        (New-Object System.Drawing.PointF -ArgumentList 27, 53),
        (New-Object System.Drawing.PointF -ArgumentList 15, 40),
        (New-Object System.Drawing.PointF -ArgumentList 3, 40)
    )
    $g.FillPolygon($white, $pts)
    $g.DrawArc($pen, 15, 20, 24, 24, -55, 110)
    $g.DrawArc($pen, 9, 14, 36, 36, -55, 110)
    $g.DrawArc($pen, 3, 8, 48, 48, -55, 110)

    $hIcon = $bmp.GetHicon()
    $icon = [System.Drawing.Icon]::FromHandle($hIcon)
    $fs = [System.IO.File]::Create($ico)
    $icon.Save($fs)
    $fs.Close()
    $g.Dispose()
    $bmp.Dispose()
    Write-Host "已生成图标 $ico"
}

# ---- 编译 ----
$exe  = Join-Path $out "DspVolumeCtl.exe"
$files = (Get-ChildItem (Join-Path $src "*.cs")).FullName

& $csc /nologo /target:winexe /platform:anycpu /optimize+ `
    "/out:$exe" `
    "/win32icon:$ico" `
    "/win32manifest:$(Join-Path $src 'app.manifest')" `
    /r:System.dll /r:System.Drawing.dll /r:System.Windows.Forms.dll `
    /r:System.Web.Extensions.dll /r:System.Net.Http.dll `
    $files

if ($LASTEXITCODE -ne 0) { throw "编译失败" }
Write-Host ""
Write-Host "OK -> $exe" -ForegroundColor Green
