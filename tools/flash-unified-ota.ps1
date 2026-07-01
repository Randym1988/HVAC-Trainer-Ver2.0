param(
  [ValidateSet("furnace", "heatpump")]
  [string]$Target,

  [ValidateRange(1, 999)]
  [int]$TrainerNumber,

  [string]$OtaHost,
  [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

$workspaceRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$firmwareDir = Join-Path $workspaceRoot "trainers\unified-master\firmware"
$instanceHeaderPath = Join-Path $firmwareDir "src\TrainerInstance.h"
$registryPath = Join-Path $workspaceRoot "tools\trainer-instance-registry.json"
$firmwareBinPath = Join-Path $firmwareDir ".pio\build\ota\firmware.bin"

if (-not (Test-Path $firmwareDir)) {
  throw "Unified firmware directory not found: $firmwareDir"
}

function Invoke-CheckedCommand {
  param(
    [Parameter(Mandatory = $true)]
    [string]$File,
    [Parameter(Mandatory = $true)]
    [string[]]$Args
  )

  & $File @Args
  if ($LASTEXITCODE -ne 0) {
    throw "Command failed ($LASTEXITCODE): $File $($Args -join ' ')"
  }
}

function Invoke-EspotaUpload {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Endpoint,
    [Parameter(Mandatory = $true)]
    [string]$BinPath
  )

  Write-Host "Attempt 1: OTA with password"
  & pio pkg exec -p tool-espotapy -- espota.py -i $Endpoint -p 3232 -a "Mitchell2019!" -f $BinPath -r
  if ($LASTEXITCODE -eq 0) {
    return
  }

  Write-Host "Attempt 2: OTA without password fallback"
  & pio pkg exec -p tool-espotapy -- espota.py -i $Endpoint -p 3232 -f $BinPath -r
  if ($LASTEXITCODE -ne 0) {
    throw "OTA upload failed after password and no-password attempts."
  }
}

function Invoke-HttpOtaUpload {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Endpoint,
    [Parameter(Mandatory = $true)]
    [string]$BinPath
  )

  Write-Host "Attempt 3: HTTP OTA fallback via /update"
  $httpBody = & curl.exe --silent --show-error -F "update=@$BinPath;type=application/octet-stream" "http://$Endpoint/update" 2>&1
  $curlExit = $LASTEXITCODE
  $bodyTrim = ([string]$httpBody).Trim()

  if ($curlExit -eq 0 -and $bodyTrim -eq "OK") {
    return
  }

  if ($curlExit -eq 56 -or $bodyTrim -eq "FAIL") {
    Write-Host "HTTP OTA returned reset/FAIL; verifying board restart..."
    Start-Sleep -Seconds 3
    for ($i = 0; $i -lt 10; $i++) {
      try {
        $null = Invoke-WebRequest -UseBasicParsing -TimeoutSec 3 "http://$Endpoint/api/status"
        Write-Host "Board responded after HTTP OTA fallback."
        return
      } catch {
        Start-Sleep -Seconds 1
      }
    }
  }

  throw "HTTP OTA upload fallback failed."
}

function Resolve-OtaEndpoint {
  param(
    [Parameter(Mandatory = $true)]
    [string]$HostName
  )

  try {
    $dns = Resolve-DnsName -Name $HostName -ErrorAction Stop |
      Where-Object { $_.IPAddress -and $_.IPAddress -match '^\d+\.\d+\.\d+\.\d+$' } |
      Select-Object -First 1
    if ($null -ne $dns) {
      return [string]$dns.IPAddress
    }
  }
  catch {
    # Fall through to .NET DNS lookup.
  }

  try {
    $ip = [System.Net.Dns]::GetHostAddresses($HostName) |
      Where-Object { $_.AddressFamily -eq [System.Net.Sockets.AddressFamily]::InterNetwork } |
      Select-Object -First 1
    if ($null -ne $ip) {
      return $ip.IPAddressToString
    }
  }
  catch {
    # Leave unresolved and return the original host.
  }

  return $HostName
}

function Load-Registry {
  if (-not (Test-Path $registryPath)) {
    throw "Board registry not found. Run tools/flash-unified.ps1 via USB once first."
  }

  $raw = Get-Content -Path $registryPath -Raw
  if ([string]::IsNullOrWhiteSpace($raw)) {
    throw "Board registry is empty."
  }

  $loadedObj = $raw | ConvertFrom-Json
  $loaded = @{ schemaVersion = 1; boards = @{} }

  if ($null -ne $loadedObj -and $null -ne $loadedObj.schemaVersion) {
    $loaded["schemaVersion"] = [int]$loadedObj.schemaVersion
  }

  if ($null -ne $loadedObj -and $null -ne $loadedObj.boards) {
    foreach ($boardProp in $loadedObj.boards.PSObject.Properties) {
      $entry = $boardProp.Value
      $entryHash = @{
        target = [string]$entry.target
        trainerNumber = [int]$entry.trainerNumber
      }
      if ($null -ne $entry.otaHost -and -not [string]::IsNullOrWhiteSpace([string]$entry.otaHost)) {
        $entryHash["otaHost"] = [string]$entry.otaHost
      }
      $loaded["boards"][$boardProp.Name.ToLowerInvariant()] = $entryHash
    }
  }

  if (-not $loaded.ContainsKey("boards") -or $loaded["boards"].Count -eq 0) {
    throw "Board registry has no entries."
  }
  return $loaded
}

function Resolve-TargetEntry {
  param(
    [Parameter(Mandatory = $true)]
    [hashtable]$Registry,
    [string]$TargetName,
    [int]$PreferredTrainerNumber
  )

  $matches = @()
  foreach ($entry in $Registry["boards"].GetEnumerator()) {
    $mac = $entry.Key
    $value = $entry.Value
    if ($value -isnot [hashtable]) {
      continue
    }

    if ($PreferredTrainerNumber -gt 0) {
      if ([int]$value["trainerNumber"] -eq $PreferredTrainerNumber) {
        $matches += @{ mac = $mac; data = $value }
      }
      continue
    }

    if ([string]::IsNullOrWhiteSpace($TargetName)) {
      continue
    }

    if ($value.ContainsKey("target") -and $value["target"] -eq $TargetName) {
      $matches += @{ mac = $mac; data = $value }
    }
  }

  if ($matches.Count -eq 0) {
    throw "No matching board found in registry. Provide -TrainerNumber explicitly."
  }

  if ($matches.Count -gt 1 -and $PreferredTrainerNumber -le 0) {
    throw "Multiple boards found for target '$TargetName'. Use -TrainerNumber to disambiguate."
  }

  return $matches[0]
}

function Write-TrainerInstanceHeader {
  param(
    [Parameter(Mandatory = $true)]
    [int]$InstanceNumber
  )

  $header = @(
    "#ifndef TRAINER_INSTANCE_H",
    "#define TRAINER_INSTANCE_H",
    "",
    "#ifndef TRAINER_INSTANCE",
    "#define TRAINER_INSTANCE $InstanceNumber",
    "#endif",
    "",
    "#endif"
  ) -join "`n"

  Set-Content -Path $instanceHeaderPath -Value $header -Encoding ascii
}

$registry = Load-Registry
$resolved = Resolve-TargetEntry -Registry $registry -TargetName $Target -PreferredTrainerNumber $TrainerNumber
$resolvedData = $resolved["data"]
$resolvedTrainerNumber = [int]$resolvedData["trainerNumber"]
$resolvedTarget = [string]$resolvedData["target"]

$resolvedHost = $OtaHost
if ([string]::IsNullOrWhiteSpace($resolvedHost)) {
  if ($resolvedData.ContainsKey("otaHost")) {
    $resolvedHost = [string]$resolvedData["otaHost"]
  } else {
    $resolvedHost = ("trainer{0}.local" -f $resolvedTrainerNumber.ToString("00"))
  }
}

Write-Host "Resolved target : $resolvedTarget"
Write-Host "Trainer number  : $resolvedTrainerNumber"
Write-Host "OTA host        : $resolvedHost"
Write-Host "Board MAC       : $($resolved["mac"])"

$uploadEndpoint = Resolve-OtaEndpoint -HostName $resolvedHost
if ($uploadEndpoint -ne $resolvedHost) {
  Write-Host "OTA endpoint    : $uploadEndpoint (resolved from hostname)"
} else {
  Write-Host "OTA endpoint    : $uploadEndpoint"
}

Push-Location $firmwareDir
try {
  # Embed the specific trainer number into the firmware at build time for simple, consistent naming.
  Write-TrainerInstanceHeader -InstanceNumber $resolvedTrainerNumber

  if (-not $SkipBuild) {
    Invoke-CheckedCommand -File "pio" -Args @("run", "-e", "ota")
  } else {
    Write-Host "Skipping build by request."
  }

  if (-not (Test-Path $firmwareBinPath)) {
    throw "Expected OTA firmware binary not found: $firmwareBinPath"
  }

  try {
    Invoke-EspotaUpload -Endpoint $uploadEndpoint -BinPath $firmwareBinPath
  } catch {
    Write-Host "ESP OTA handshake failed, attempting HTTP OTA fallback..."
    Invoke-HttpOtaUpload -Endpoint $uploadEndpoint -BinPath $firmwareBinPath
  }
  Write-Host "OTA update complete for Trainer $resolvedTrainerNumber on $resolvedHost ($uploadEndpoint)."
}
finally {
  Pop-Location
}
