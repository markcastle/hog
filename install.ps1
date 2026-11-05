# hog installer
#
# Usage (PowerShell, no admin needed):
#   irm https://raw.githubusercontent.com/markcastle/hog/master/install.ps1 | iex
#
# What it does:
#   1. Downloads the latest hog.exe release (x64, or x86 on 32-bit Windows)
#   2. Puts it in %LOCALAPPDATA%\hog
#   3. Adds that folder to your *user* PATH (no admin, no system changes)
#
# Re-run it any time to upgrade. Uninstall = delete the folder and remove it from PATH.

$ErrorActionPreference = 'Stop'

$repo    = 'markcastle/hog'
$dir     = Join-Path $env:LOCALAPPDATA 'hog'
$exeName = if ([Environment]::Is64BitOperatingSystem) { 'hog.exe' } else { 'hog-x86.exe' }
$url     = "https://github.com/$repo/releases/latest/download/$exeName"
$dest    = Join-Path $dir 'hog.exe'

# Older PowerShell defaults to TLS 1.0, which GitHub rejects.
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

Write-Host "Downloading $exeName from latest release..."
New-Item -ItemType Directory -Force -Path $dir | Out-Null
Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing

# Add to user PATH if it isn't there already.
$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
if (($userPath -split ';') -notcontains $dir) {
    [Environment]::SetEnvironmentVariable('Path', "$userPath;$dir", 'User')
    Write-Host "Added $dir to your user PATH."
}
# Make it work in this session too, not just new windows.
if (($env:Path -split ';') -notcontains $dir) { $env:Path += ";$dir" }

$ver = & $dest -v
Write-Host ""
Write-Host "Installed $ver to $dest"
Write-Host "Open a new terminal (or just use this one) and type: hog"
