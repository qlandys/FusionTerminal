param(
  [string]$ExePath = "releases/win64/FusionTerminal.exe",
  [string]$BackendExePath = "releases/win64/orderbook_backend.exe",
  [string]$OutPath = "latest.json",
  [string]$Repo = "qlandys/FusionTerminal",
  [string]$Version = "",
  [string]$Tag = "",
  [string]$AssetName = "",
  [string]$BackendAssetName = "orderbook_backend.exe",
  [string]$ReleaseName = "",
  [string]$ReleaseNotes = "",
  [switch]$BumpVersion,
  [switch]$UploadRelease,
  [switch]$ReplaceAssets,
  [switch]$PushGit,
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
  $suffix = ""
  if ($ver) {
    $dash = $ver.IndexOf("-")
    if ($dash -ge 0) {
      $suffix = $ver.Substring($dash)
    }
  }
  $clean = Normalize-Version $ver
  $m = [regex]::Match($clean, '^(?<maj>\d+)\.(?<min>\d+)\.(?<patch>\d+)$')
  if ($m.Success) {
    $maj = [int]$m.Groups["maj"].Value
    $min = [int]$m.Groups["min"].Value
    $patch = [int]$m.Groups["patch"].Value
    $patch = $patch + 1
    return "$maj.$min.$patch$suffix"
  }
  return $ver
}

function Get-GitHubToken() {
  if ($env:GITHUB_TOKEN) {
    return $env:GITHUB_TOKEN
  }
  if ($env:GH_TOKEN) {
    return $env:GH_TOKEN
  }
  $tokenPaths = @(
    (Join-Path $PSScriptRoot "..\\..\\.fusion_token"),
    (Join-Path (Get-Location) ".fusion_token")
  )
  foreach ($tokenPath in $tokenPaths) {
    if (Test-Path $tokenPath) {
      $content = Get-Content -Path $tokenPath -Raw -ErrorAction SilentlyContinue
      if ($null -ne $content) {
        $token = $content.Trim()
        if ($token) {
          return $token
        }
      }
    }
  }
  return ""
}

function Get-ReleaseByTag([string]$repo, [string]$tag, [string]$token) {
  $headers = @{
    "Accept" = "application/vnd.github+json"
    "User-Agent" = "FusionTerminal-Release"
    "Authorization" = "Bearer $token"
  }
  $url = "https://api.github.com/repos/$repo/releases/tags/$tag"
  try {
    return Invoke-RestMethod -Headers $headers -Uri $url -Method Get
  } catch {
    return $null
  }
}

function Create-Release([string]$repo, [string]$tag, [string]$name, [string]$notes, [string]$token) {
  $headers = @{
    "Accept" = "application/vnd.github+json"
    "User-Agent" = "FusionTerminal-Release"
    "Authorization" = "Bearer $token"
  }
  $payload = @{
    tag_name = $tag
    name = $name
    body = $notes
    draft = $false
    prerelease = $false
  } | ConvertTo-Json -Depth 3

  $url = "https://api.github.com/repos/$repo/releases"
  return Invoke-RestMethod -Headers $headers -Uri $url -Method Post -Body $payload
}

function Upload-ReleaseAsset([int]$releaseId, [string]$repo, [string]$path, [string]$name, [string]$token, [switch]$ReplaceAssets) {
  if (-not (Test-Path $path)) {
    throw "Asset not found: $path"
  }
  $headers = @{
    "Accept" = "application/vnd.github+json"
    "User-Agent" = "FusionTerminal-Release"
    "Authorization" = "Bearer $token"
  }
  if ($ReplaceAssets) {
    $assetsUrl = "https://api.github.com/repos/$repo/releases/$releaseId/assets"
    $assets = Invoke-RestMethod -Headers $headers -Uri $assetsUrl -Method Get
    foreach ($asset in $assets) {
      if ($asset.name -eq $name) {
        Invoke-RestMethod -Headers $headers -Uri $asset.url -Method Delete
        break
      }
    }
  }

  $uploadUrl = "https://uploads.github.com/repos/$repo/releases/$releaseId/assets?name=$name"
  $headers["Content-Type"] = "application/octet-stream"
  return Invoke-RestMethod -Headers $headers -Uri $uploadUrl -Method Post -InFile $path
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

if ($UploadRelease) {
  $token = Get-GitHubToken
  if (-not $token) {
    throw "GITHUB_TOKEN or GH_TOKEN is required for -UploadRelease."
  }
  if (-not $ReleaseName) {
    $ReleaseName = $Tag
  }

  $release = Get-ReleaseByTag $Repo $Tag $token
  if (-not $release) {
    $release = Create-Release $Repo $Tag $ReleaseName $ReleaseNotes $token
  }

  Upload-ReleaseAsset $release.id $Repo $ExePath $AssetName $token -ReplaceAssets:$ReplaceAssets | Out-Null
  if ($BackendExePath -and (Test-Path $BackendExePath)) {
    Upload-ReleaseAsset $release.id $Repo $BackendExePath $BackendAssetName $token -ReplaceAssets:$ReplaceAssets | Out-Null
  }
  Write-Host "Release uploaded: $Tag" -ForegroundColor Green
}

if ($PushGit) {
  & git add -A
  if ($LASTEXITCODE -ne 0) {
    throw "git add failed."
  }
  & git commit -m $Version
  if ($LASTEXITCODE -ne 0) {
    Write-Host "git commit skipped (no changes)." -ForegroundColor Yellow
  }
  & git push
  if ($LASTEXITCODE -ne 0) {
    throw "git push failed."
  }
  Write-Host "Git pushed (commit: $Version)." -ForegroundColor Green
}
