$ErrorActionPreference = "Stop"

Set-Location (Split-Path -Parent $MyInvocation.MyCommand.Path) | Out-Null
Set-Location .. | Out-Null
Set-Location .. | Out-Null

$buildDir = "build"
$releaseDir = "releases/win64"

Write-Host "[1/4] Configure (CMake)..." -ForegroundColor Cyan
cmake -S . -B $buildDir | Out-Host

Write-Host "[2/4] Build (Release)..." -ForegroundColor Cyan
cmake --build $buildDir --config Release --target FusionTerminal orderbook_backend FusionUpdater | Out-Host

Write-Host "[3/4] Copy EXEs..." -ForegroundColor Cyan
New-Item -ItemType Directory -Force -Path $releaseDir | Out-Null
Copy-Item -Force "$buildDir/Release/FusionTerminal.exe" "$releaseDir/FusionTerminal.exe"
Copy-Item -Force "$buildDir/Release/orderbook_backend.exe" "$releaseDir/orderbook_backend.exe"
if (Test-Path "$buildDir/Release/FusionUpdater.exe") {
  Copy-Item -Force "$buildDir/Release/FusionUpdater.exe" "$releaseDir/FusionUpdater.exe"
}
if (Test-Path "$releaseDir/PlasmaTerminal.exe") { Remove-Item -Force "$releaseDir/PlasmaTerminal.exe" }

Write-Host "[3.5/4] Copy assets..." -ForegroundColor Cyan
if (Test-Path "$releaseDir/assets") { Remove-Item -Recurse -Force "$releaseDir/assets" }
Copy-Item -Recurse -Force "assets" "$releaseDir/assets"

Write-Host "[4/4] Deploy Qt + runtimes..." -ForegroundColor Cyan
$windeployqt = $null
try {
  $windeployqt = (Get-Command windeployqt -ErrorAction Stop).Source
} catch {
  $qtCandidates = @(
    "C:\Qt\*\msvc2022_64\bin\windeployqt.exe",
    "C:\Qt\*\msvc2019_64\bin\windeployqt.exe"
  )
  foreach ($glob in $qtCandidates) {
    $found = Get-ChildItem $glob -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1
    if ($found) { $windeployqt = $found.FullName; break }
  }
}

if (-not $windeployqt) {
  throw "windeployqt.exe not found (add Qt bin to PATH or install Qt)."
}

& $windeployqt --release --no-translations --no-system-d3d-compiler --dir $releaseDir "$buildDir/Release/FusionTerminal.exe" | Out-Host

# MSVC runtime DLLs (so it runs on clean Windows without VC++ redist installed)
$msvcRuntime = @(
  "vcruntime140.dll",
  "vcruntime140_1.dll",
  "msvcp140.dll",
  "concrt140.dll",
  "msvcp140_1.dll",
  "msvcp140_2.dll"
)
foreach ($dll in $msvcRuntime) {
  $src = Join-Path $env:WINDIR ("System32\" + $dll)
  if (Test-Path $src) {
    Copy-Item -Force $src (Join-Path $releaseDir $dll)
  }
}

Write-Host "OK: $releaseDir" -ForegroundColor Green
