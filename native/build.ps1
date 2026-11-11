param(
    [string]$CloudflaredPath = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$outDir = Join-Path $root "build"
$thirdPartyDir = Join-Path $root "third_party"
$cfTarget = Join-Path $thirdPartyDir "cloudflared.exe"

New-Item -ItemType Directory -Path $outDir, $thirdPartyDir -Force | Out-Null

if ($CloudflaredPath) {
    if (-not (Test-Path $CloudflaredPath)) {
        throw "Specified cloudflared.exe not found: $CloudflaredPath"
    }
    Copy-Item $CloudflaredPath $cfTarget -Force
} elseif (-not (Test-Path $cfTarget)) {
    $url = "https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-windows-amd64.exe"
    Write-Host "Downloading cloudflared.exe from $url..."
    Invoke-WebRequest -Uri $url -OutFile $cfTarget -UseBasicParsing
}

$resObj = Join-Path $outDir "app.res.o"
& windres.exe (Join-Path $root "resources\app.rc") -O coff -o $resObj
if ($LASTEXITCODE -ne 0) {
    throw "Resource compilation failed with exit code $LASTEXITCODE"
}

$sources = @(
    (Join-Path $root "src\main.cpp"),
    (Join-Path $root "src\settings.cpp"),
    (Join-Path $root "src\tunnel.cpp"),
    (Join-Path $root "src\overlay.cpp"),
    (Join-Path $root "src\ui_windows.cpp")
)

$outExe = Join-Path $outDir "LensAnywhere.exe"
$args = @(
    "-std=c++20", "-O2", "-s", "-municode", "-mwindows",
    "-DUNICODE", "-D_UNICODE", "-D_WIN32_WINNT=0x0A00", "-DWINVER=0x0A00"
) + $sources + @(
    $resObj,
    "-o", $outExe,
    "-lgdiplus", "-lws2_32", "-lwinhttp", "-ldwmapi",
    "-lole32", "-lshell32", "-lcomctl32", "-luxtheme", "-lshlwapi", "-luuid"
)

& g++.exe @args
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

Get-Item $outExe | Select-Object FullName, Length, LastWriteTime
