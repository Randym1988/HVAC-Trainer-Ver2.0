param(
    [string]$Destination = (Join-Path $env:USERPROFILE "Documents\HVAC-Trainer-Backups"),
    [int]$Keep = 14
)

# Backs up files that are git-ignored and cannot be rebuilt from the repository:
# user database, edge registry, MQTT password file, and the credentials file.
# The archive contains secrets: store it somewhere private, ideally a second drive.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$sources = @(
    "platform\docker-engine\backend\data",
    "platform\docker-engine\mosquitto\data",
    "platform\docker-engine\.env",
    "tools\trainer-login-credentials.txt"
) | ForEach-Object { Join-Path $root $_ } | Where-Object { Test-Path $_ }

if (-not $sources) { throw "Nothing to back up." }
New-Item -ItemType Directory -Force -Path $Destination | Out-Null
$archive = Join-Path $Destination ("hvac-trainer-backup-{0:yyyyMMdd-HHmmss}.zip" -f (Get-Date))
Compress-Archive -Path $sources -DestinationPath $archive -CompressionLevel Optimal
Write-Host "Backup written: $archive"

Get-ChildItem $Destination -Filter "hvac-trainer-backup-*.zip" |
    Sort-Object LastWriteTime -Descending |
    Select-Object -Skip $Keep |
    Remove-Item -Force
