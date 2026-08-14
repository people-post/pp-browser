# Wipe local pp-browser profile data after Brief PQ / Account ID hard cut.
#
# Windows. Linux/macOS: scripts/wipe_local_profile.sh
# Android / iOS: clear app storage by hand.
#
# Default data root: %LOCALAPPDATA%\pp-browser\
# See docs/contracts/DATA_LAYOUT.md
#
# Examples:
#   .\scripts\wipe_local_profile.ps1 -DryRun
#   .\scripts\wipe_local_profile.ps1 -Yes
#   .\scripts\wipe_local_profile.ps1 -Yes -Profile default
#   .\scripts\wipe_local_profile.ps1 -Yes -DataDir "D:\pp-data"

[CmdletBinding()]
param(
  [switch]$Yes,
  [switch]$DryRun,
  [string]$Profile = "",
  [string]$DataDir = "",
  [switch]$KeepRegistry
)

$ErrorActionPreference = "Stop"

if (-not $Yes -and -not $DryRun) {
  $DryRun = $true
}
if ($Yes) {
  $DryRun = $false
}

if ([string]::IsNullOrWhiteSpace($DataDir)) {
  if ([string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
    Write-Error "LOCALAPPDATA is not set"
  }
  $DataDir = Join-Path $env:LOCALAPPDATA "pp-browser"
}

$ProfilesDir = Join-Path $DataDir "profiles"
$ProfilesJson = Join-Path $DataDir "profiles.json"

Write-Host "data_dir: $DataDir"

if (-not (Test-Path -LiteralPath $DataDir)) {
  Write-Host "nothing to wipe — data dir missing"
  exit 0
}

$targets = New-Object System.Collections.Generic.List[string]
if (-not [string]::IsNullOrWhiteSpace($Profile)) {
  $targets.Add((Join-Path $ProfilesDir $Profile)) | Out-Null
} else {
  if (Test-Path -LiteralPath $ProfilesDir) {
    $targets.Add($ProfilesDir) | Out-Null
  }
  if (-not $KeepRegistry -and (Test-Path -LiteralPath $ProfilesJson)) {
    $targets.Add($ProfilesJson) | Out-Null
  }
}

if ($targets.Count -eq 0) {
  Write-Host "nothing to wipe — no profiles under $ProfilesDir"
  exit 0
}

Write-Host "targets:"
foreach ($t in $targets) {
  if (Test-Path -LiteralPath $t) {
    Write-Host "  $t"
  } else {
    Write-Host "  $t (missing)"
  }
}

if ($DryRun -or -not $Yes) {
  Write-Host "dry-run complete — re-run with -Yes to delete"
  exit 0
}

foreach ($t in $targets) {
  if (Test-Path -LiteralPath $t) {
    Remove-Item -LiteralPath $t -Recurse -Force
    Write-Host "removed $t"
  }
}

Write-Host "wipe complete — restart pp-browser and re-register"
