param(
  [Parameter(Mandatory = $true)]
  [ValidateSet("furnace", "heatpump")]
  [string]$Target,

  [Parameter(Mandatory = $true)]
  [string]$Port,

  [ValidateRange(1, 999)]
  [int]$TrainerNumber,

  [switch]$Yes,
  [switch]$Force,
  [switch]$SkipMacCheck,
  [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
if ($PSVersionTable.PSVersion.Major -ge 7) {
  $PSNativeCommandUseErrorActionPreference = $false
}

$expectedMacByTarget = @{
  furnace  = "c0:4e:30:34:4b:e4"
  heatpump = "3c:84:27:f8:03:1c"
}

$gpio14ByTarget = @{
  furnace  = "GPIO14 LOW (tied to GND)"
  heatpump = "GPIO14 HIGH (open/floating, internal pull-up)"
}

$workspaceRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$firmwareDir = Join-Path $workspaceRoot "trainers\unified-master\firmware"
$instanceHeaderPath = Join-Path $firmwareDir "src\TrainerInstance.h"
$registryPath = Join-Path $workspaceRoot "tools\trainer-instance-registry.json"

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

  $oldEap = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  & $File @Args
  $ErrorActionPreference = $oldEap
  if ($LASTEXITCODE -ne 0) {
    throw "Command failed ($LASTEXITCODE): $File $($Args -join ' ')"
  }
}

function Get-BoardMac {
  param(
    [Parameter(Mandatory = $true)]
    [string]$PortName
  )

  $oldEap = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  $output = & pio pkg exec -p tool-esptoolpy -- python -m esptool --port $PortName read_mac 2>$null
  if ($LASTEXITCODE -ne 0) {
    $output = & pio pkg exec -p tool-esptoolpy -- esptool.py --port $PortName read_mac 2>$null
  }
  $ErrorActionPreference = $oldEap
  if ($LASTEXITCODE -ne 0) {
    throw "Unable to read MAC on port $PortName. Esptool output: $($output -join "`n")"
  }

  $text = ($output -join "`n")
  $match = [regex]::Match($text, "MAC:\s*([0-9a-fA-F:]{17})")
  if (-not $match.Success) {
    throw "Could not parse board MAC from esptool output."
  }

  return $match.Groups[1].Value.ToLowerInvariant()
}

function Load-Registry {
  if (-not (Test-Path $registryPath)) {
    return @{
      schemaVersion = 1
      boards = @{
        "c0:4e:30:34:4b:e4" = @{ target = "furnace"; trainerNumber = 1 }
        "3c:84:27:f8:03:1c" = @{ target = "heatpump"; trainerNumber = 2 }
      }
    }
  }

  $raw = Get-Content -Path $registryPath -Raw
  if ([string]::IsNullOrWhiteSpace($raw)) {
    return @{ schemaVersion = 1; boards = @{} }
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

  return $loaded
}

function Save-Registry {
  param(
    [Parameter(Mandatory = $true)]
    [hashtable]$Registry
  )

  $json = $Registry | ConvertTo-Json -Depth 6
  Set-Content -Path $registryPath -Value $json -Encoding ascii
}

function Get-OtaHostFromTrainerNumber {
  param(
    [Parameter(Mandatory = $true)]
    [int]$InstanceNumber
  )

  return ("trainer{0}.local" -f $InstanceNumber.ToString("00"))
}

function Resolve-TrainerNumber {
  param(
    [Parameter(Mandatory = $true)]
    [hashtable]$Registry,
    [Parameter(Mandatory = $true)]
    [string]$Mac,
    [Parameter(Mandatory = $true)]
    [string]$TargetName,
    [int]$PreferredNumber
  )

  if (-not $Registry.ContainsKey("boards")) {
    $Registry["boards"] = @{}
  }

  $boards = $Registry["boards"]

  if ($PreferredNumber -gt 0) {
    return $PreferredNumber
  }

  if ($boards.ContainsKey($Mac)) {
    $entry = $boards[$Mac]
    if ($entry -is [hashtable] -and $entry.ContainsKey("trainerNumber")) {
      return [int]$entry["trainerNumber"]
    }
  }

  $maxAssigned = 0
  foreach ($boardEntry in $boards.Values) {
    if ($boardEntry -is [hashtable] -and $boardEntry.ContainsKey("trainerNumber")) {
      $n = [int]$boardEntry["trainerNumber"]
      if ($n -gt $maxAssigned) {
        $maxAssigned = $n
      }
    }
  }

  if ($maxAssigned -lt 1) {
    if ($TargetName -eq "heatpump") {
      return 2
    }
    return 1
  }

  return ($maxAssigned + 1)
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

Write-Host "Target profile : $Target"
Write-Host "Upload port    : $Port"
Write-Host "GPIO14 strap   : $($gpio14ByTarget[$Target])"
Write-Host "Firmware path  : $firmwareDir"

if (-not $Yes) {
  $confirm = Read-Host "Type the target profile name to continue"
  if ($confirm -ne $Target) {
    throw "Confirmation mismatch. Expected '$Target'."
  }
}

Push-Location $firmwareDir
try {
  $registry = Load-Registry
  $detectedMac = ""

  if (-not $SkipMacCheck) {
    $detectedMac = Get-BoardMac -PortName $Port
    $expectedMac = $expectedMacByTarget[$Target]

    Write-Host "Detected MAC   : $detectedMac"
    Write-Host "Expected MAC   : $expectedMac"

    if (($detectedMac -ne $expectedMac) -and (-not $Force)) {
      throw "Board MAC does not match target '$Target'. Use -Force to override intentionally."
    }
  } else {
    Write-Host "Skipping MAC check by request."
    $detectedMac = Get-BoardMac -PortName $Port
    Write-Host "Detected MAC   : $detectedMac"
  }

  $resolvedTrainerNumber = Resolve-TrainerNumber -Registry $registry -Mac $detectedMac -TargetName $Target -PreferredNumber $TrainerNumber
  $resolvedOtaHost = Get-OtaHostFromTrainerNumber -InstanceNumber $resolvedTrainerNumber
  Write-Host "Trainer number : $resolvedTrainerNumber"
  Write-Host "OTA hostname   : $resolvedOtaHost"

  Write-TrainerInstanceHeader -InstanceNumber $resolvedTrainerNumber

  if (-not $registry.ContainsKey("boards")) {
    $registry["boards"] = @{}
  }
  $registry["boards"][$detectedMac] = @{ target = $Target; trainerNumber = $resolvedTrainerNumber; otaHost = $resolvedOtaHost }
  Save-Registry -Registry $registry

  if (-not $SkipBuild) {
    Invoke-CheckedCommand -File "pio" -Args @("run", "-e", "usb")
  } else {
    Write-Host "Skipping build by request."
  }

  Invoke-CheckedCommand -File "pio" -Args @("run", "-e", "usb", "-t", "upload", "--upload-port", $Port)
  Write-Host "Unified firmware upload complete for target '$Target' (Trainer $resolvedTrainerNumber)."
  Write-Host "Next OTA command: .\tools\flash-unified-ota.ps1 -TrainerNumber $resolvedTrainerNumber"
}
finally {
  Pop-Location
}
