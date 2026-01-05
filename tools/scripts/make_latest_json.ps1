param(
  [string]$ExePath = "releases/win64/FusionTerminal.exe",
  [string]$BackendExePath = "releases/win64/orderbook_backend.exe",
  [string]$OutPath = "latest.json",
  [string]$Repo = "qlandys/FusionTerminal",
  [string]$Version = "",
  [string]$Tag = "",
  [string]$AssetName = "",
  [string]$BackendAssetName = "orderbook_backend.exe",
  [switch]$BumpVersion,
  [switch]$NoWrite
)

$ErrorActionPreference = "Stop"

Set-Location (Split-Path -Parent $MyInvocation.MyCommand.Path) | Out-Null
Set-Location .. | Out-Null
Set-Location .. | Out-Null

function Get-VersionFromMainCpp([string]$path) {
  if (-not (Test-Path $path)) {
    return ""
  }
  $content = Get-Content -Path $path -Raw
  $m = [regex]::Match($content, 'setApplicationVersion\s*\(\s*QStringLiteral\("([^"]+)"\)\s*\)')
  if ($m.Success) {
    return $m.Groups[1].Value
  }
  return ""
}

function Get-VersionFromLatestJson([string]$path) {
  if (-not (Test-Path $path)) {
    return ""
  }
  try {
    $json = Get-Content -Path $path -Raw | ConvertFrom-Json
    if ($null -ne $json.version) {
      return [string]$json.version
    }
  } catch {
    return ""
  }
  return ""
}

function Normalize-Version([string]$ver) {
  if (-not $ver) {
    return ""
  }
  $v = $ver.Trim()
  if ($v.StartsWith("v")) {
    $v = $v.Substring(1)
  }
  $dash = $v.IndexOf("-")
  if ($dash -ge 0) {
    $v = $v.Substring(0, $dash)
  }
  return $v
}

function Bump-Version([string]$ver) {
  $clean = Normalize-Version $ver
  $m = [regex]::Match($clean, '^(?<maj>\d+)\.(?<min>\d+)\.(?<patch>\d+)$')
  if ($m.Success) {
    $maj = [int]$m.Groups["maj"].Value
    $min = [int]$m.Groups["min"].Value
    $patch = [int]$m.Groups["patch"].Value
    $patch = $patch + 1
    return "$maj.$min.$patch"
  }
  return $ver
}

$mainCpp = "gui_native/main.cpp"
if (-not $Version) {
  $Version = Get-VersionFromMainCpp $mainCpp
}
if (-not $Version) {
  $Version = Get-VersionFromLatestJson $OutPath
}

if ($BumpVersion) {
  if (-not $Version) {
    throw "Version not set. Pass -Version or set QCoreApplication::setApplicationVersion in gui_native/main.cpp or latest.json."
  }
  $nextVersion = Bump-Version $Version
  if ($nextVersion -ne $Version) {
    if (Test-Path $mainCpp) {
      $content = Get-Content -Path $mainCpp -Raw
      $updated = [regex]::Replace(
        $content,
        'setApplicationVersion\s*\(\s*QStringLiteral\("([^"]+)"\)\s*\)',
        "setApplicationVersion(QStringLiteral(`"$nextVersion`"))",
        1
      )
      if ($updated -ne $content) {
        Set-Content -Path $mainCpp -Value $updated -Encoding UTF8
      }
    }
    $Version = $nextVersion
  }
}

if (-not $Version) {
  throw "Version not set. Pass -Version or set QCoreApplication::setApplicationVersion in gui_native/main.cpp."
}

if (-not $Tag) {
  $Tag = $Version
}

if (-not $AssetName) {
  $AssetName = "FusionTerminal-$Version.exe"
}

if ($NoWrite) {
  Write-Host "Version: $Version" -ForegroundColor Cyan
  return
}

if (-not (Test-Path $ExePath)) {
  throw "Exe not found: $ExePath"
}

$hash = (Get-FileHash -Algorithm SHA256 -Path $ExePath).Hash.ToLower()

$url = "https://github.com/$Repo/releases/download/$Tag/$AssetName"

$backendHash = ""
$backendUrl = ""
if ($BackendExePath -and (Test-Path $BackendExePath)) {
  $backendHash = (Get-FileHash -Algorithm SHA256 -Path $BackendExePath).Hash.ToLower()
  $backendUrl = "https://github.com/$Repo/releases/download/$Tag/$BackendAssetName"
}

$payload = [ordered]@{
  version   = $Version
  build     = 1
  mandatory = $false
  url       = $url
  sha256    = $hash
  backend_url    = $backendUrl
  backend_sha256 = $backendHash
  notes     = @("Auto-generated manifest.")
}

$json = $payload | ConvertTo-Json -Depth 4
Set-Content -Path $OutPath -Value $json -Encoding UTF8

Write-Host "Wrote $OutPath (version=$Version, sha256=$hash)" -ForegroundColor Green
