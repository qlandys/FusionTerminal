$ErrorActionPreference = "Stop"

Set-Location (Split-Path -Parent $MyInvocation.MyCommand.Path) | Out-Null
Set-Location .. | Out-Null
Set-Location .. | Out-Null

Write-Host "[1/3] Bump version (main.cpp)..." -ForegroundColor Cyan
& "$PSScriptRoot/make_latest_json.ps1" -BumpVersion -NoWrite | Out-Host

Write-Host "[2/3] Build + package..." -ForegroundColor Cyan
& "$PSScriptRoot/package_win64.ps1" | Out-Host

Write-Host "[3/3] Publish release + push..." -ForegroundColor Cyan
& "$PSScriptRoot/make_latest_json.ps1" -UploadRelease -ReplaceAssets -PushGit | Out-Host

Write-Host "Release complete." -ForegroundColor Green
