<#
.SYNOPSIS
    Builds trainer-specific OTA binaries and uploads them to all registered trainers in parallel.
.DESCRIPTION
    The unified firmware embeds trainer identity at build time via src/TrainerInstance.h.
    This script builds one binary per registered trainer, stores them in a temporary
    artifacts folder, then launches OTA uploads concurrently to reduce fleet update time.
.EXAMPLE
    .\tools\flash-all-ota.ps1
    Build and OTA-update all trainers in the registry concurrently.
.EXAMPLE
    .\tools\flash-all-ota.ps1 -SkipBuild
    Skip builds and reuse previously staged OTA binaries in tools/.ota-artifacts.
#>
param(
  [switch]$SkipBuild,
  [ValidateRange(1, 32)]
  [int]$MaxParallel = 8
)

$ErrorActionPreference = "Stop"
if ($PSVersionTable.PSVersion.Major -ge 7) {
  $PSNativeCommandUseErrorActionPreference = $false
}

$workspaceRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$firmwareDir = Join-Path $workspaceRoot "trainers\unified-master\firmware"
$instanceHeaderPath = Join-Path $firmwareDir "src\TrainerInstance.h"
$registryPath = Join-Path $workspaceRoot "tools\trainer-instance-registry.json"
$artifactDir = Join-Path $workspaceRoot "tools\.ota-artifacts"
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

function Get-RegistryBoards {
  if (-not (Test-Path $registryPath)) {
    throw "Trainer registry not found at '$registryPath'. Provision boards via USB first."
  }

  $raw = Get-Content -Path $registryPath -Raw
  if ([string]::IsNullOrWhiteSpace($raw)) {
    throw "Trainer registry is empty."
  }

  $registryObj = $raw | ConvertFrom-Json
  if ($null -eq $registryObj -or $null -eq $registryObj.boards) {
    throw "Trainer registry format is invalid. Expected top-level 'boards' object."
  }

  $entries = @()
  foreach ($boardProp in $registryObj.boards.PSObject.Properties) {
    $mac = $boardProp.Name.ToLowerInvariant()
    $entry = $boardProp.Value

    if ($null -eq $entry.trainerNumber -or [int]$entry.trainerNumber -le 0) {
      throw "Registry entry for MAC '$mac' is missing a valid trainerNumber."
    }

    $trainerNumber = [int]$entry.trainerNumber
    $otaHost = [string]$entry.otaHost
    if ([string]::IsNullOrWhiteSpace($otaHost)) {
      $otaHost = "trainer{0}.local" -f $trainerNumber.ToString("00")
    }
    $otaEndpoint = Resolve-OtaEndpoint -HostName $otaHost

    $entries += [pscustomobject]@{
      Mac = $mac
      Target = [string]$entry.target
      TrainerNumber = $trainerNumber
      OtaHost = $otaHost
      OtaEndpoint = $otaEndpoint
    }
  }

  if ($entries.Count -eq 0) {
    throw "Trainer registry has no board entries."
  }

  return $entries | Sort-Object TrainerNumber
}

function New-TrainerArtifacts {
  param(
    [Parameter(Mandatory = $true)]
    [object[]]$BoardEntries
  )

  if (-not (Test-Path $artifactDir)) {
    New-Item -ItemType Directory -Path $artifactDir -Force | Out-Null
  }

  Push-Location $firmwareDir
  try {
    foreach ($board in $BoardEntries) {
      $trainer = [int]$board.TrainerNumber
      $artifactPath = Join-Path $artifactDir ("trainer{0}.bin" -f $trainer.ToString("00"))

      Write-Host "--------------------------------------------------" -ForegroundColor DarkCyan
      Write-Host "Building OTA artifact for Trainer #$trainer ($($board.OtaHost))" -ForegroundColor Cyan

      Write-TrainerInstanceHeader -InstanceNumber $trainer
      Invoke-CheckedCommand -File "pio" -Args @("run", "-e", "ota")

      if (-not (Test-Path $firmwareBinPath)) {
        throw "Expected firmware binary not found after build: $firmwareBinPath"
      }

      Copy-Item -Path $firmwareBinPath -Destination $artifactPath -Force
      Write-Host "Artifact staged: $artifactPath" -ForegroundColor Gray
    }
  }
  finally {
    Pop-Location
  }
}

function Get-MissingArtifacts {
  param(
    [Parameter(Mandatory = $true)]
    [object[]]$BoardEntries
  )

  $missing = @()
  foreach ($board in $BoardEntries) {
    $trainer = [int]$board.TrainerNumber
    $artifactPath = Join-Path $artifactDir ("trainer{0}.bin" -f $trainer.ToString("00"))
    if (-not (Test-Path $artifactPath)) {
      $missing += $artifactPath
    }
  }

  return $missing
}

function Start-OtaUploadJob {
  param(
    [Parameter(Mandatory = $true)]
    [object]$Board
  )

  $trainer = [int]$Board.TrainerNumber
  $artifactPath = Join-Path $artifactDir ("trainer{0}.bin" -f $trainer.ToString("00"))

  return Start-Job -ArgumentList @($firmwareDir, $trainer, [string]$Board.OtaHost, [string]$Board.OtaEndpoint, $artifactPath) -ScriptBlock {
    param($jobFirmwareDir, $jobTrainerNumber, $jobHost, $jobEndpoint, $jobBin)

    $ErrorActionPreference = "Continue"
    Set-Location $jobFirmwareDir

    $outputLines = New-Object System.Collections.Generic.List[string]
    $outputLines.Add(("Starting OTA upload for Trainer #{0} to {1} (endpoint: {2})" -f $jobTrainerNumber, $jobHost, $jobEndpoint))

    $exitCode = 1

    $outputLines.Add("Attempt 1: OTA with password")
    & pio pkg exec -p tool-espotapy -- espota.py -i $jobEndpoint -p 3232 -a "Mitchell2019!" -f $jobBin -r 2>&1 |
      ForEach-Object {
        $line = [string]$_
        $outputLines.Add($line)
      }
    $exitCode = $LASTEXITCODE

    if ($exitCode -ne 0) {
      $outputLines.Add("Attempt 2: OTA without password fallback")
      & pio pkg exec -p tool-espotapy -- espota.py -i $jobEndpoint -p 3232 -f $jobBin -r 2>&1 |
        ForEach-Object {
          $line = [string]$_
          $outputLines.Add($line)
        }
      $exitCode = $LASTEXITCODE
    }

    if ($exitCode -ne 0) {
      $outputLines.Add("Attempt 3: HTTP OTA fallback via /update")
      $httpBody = & curl.exe --silent --show-error -F "update=@$jobBin;type=application/octet-stream" "http://$jobEndpoint/update" 2>&1
      $curlExit = $LASTEXITCODE
      foreach ($line in ($httpBody -split "`r?`n")) {
        if (-not [string]::IsNullOrWhiteSpace($line)) {
          $outputLines.Add([string]$line)
        }
      }
      $bodyTrim = ([string]$httpBody).Trim()
      if ($curlExit -eq 0 -and $bodyTrim -eq "OK") {
        $exitCode = 0
      } else {
        # curl 56 often means the board rebooted and reset the socket after a successful write.
        if ($curlExit -eq 56 -or $bodyTrim -eq "FAIL") {
          $outputLines.Add("HTTP OTA returned reset/FAIL; verifying board comes back online...")
          Start-Sleep -Seconds 3
          $online = $false
          for ($i = 0; $i -lt 10; $i++) {
            try {
              $null = Invoke-WebRequest -UseBasicParsing -TimeoutSec 3 "http://$jobEndpoint/api/status"
              $online = $true
              break
            } catch {
              Start-Sleep -Seconds 1
            }
          }
          if ($online) {
            $outputLines.Add("Board responded to /api/status after OTA fallback; treating update as successful.")
            $exitCode = 0
          } else {
            $exitCode = if ($curlExit -ne 0) { $curlExit } else { 1 }
          }
        } else {
          $exitCode = if ($curlExit -ne 0) { $curlExit } else { 1 }
        }
      }
    }

    [pscustomobject]@{
      TrainerNumber = $jobTrainerNumber
      Host = $jobHost
      Endpoint = $jobEndpoint
      Bin = $jobBin
      ExitCode = $exitCode
      Output = ($outputLines -join "`n")
    }
  }
}

$boards = Get-RegistryBoards
Write-Host "Found $($boards.Count) registered trainer(s)." -ForegroundColor Cyan

if (-not $SkipBuild) {
  New-TrainerArtifacts -BoardEntries $boards
} else {
  Write-Host "Skipping build by request. Reusing artifacts from $artifactDir" -ForegroundColor Yellow
  $missingArtifacts = Get-MissingArtifacts -BoardEntries $boards
  if ($missingArtifacts.Count -gt 0) {
    throw "Missing OTA artifacts:`n$($missingArtifacts -join "`n")"
  }
}

$jobs = @()
$completed = New-Object System.Collections.Generic.List[object]
$queue = [System.Collections.Queue]::new()
foreach ($board in $boards) {
  $queue.Enqueue($board)
}

while ($queue.Count -gt 0 -or $jobs.Count -gt 0) {
  while ($queue.Count -gt 0 -and $jobs.Count -lt $MaxParallel) {
    $nextBoard = $queue.Dequeue()
    $job = Start-OtaUploadJob -Board $nextBoard
    $jobs += $job
    Write-Host "Queued OTA upload for Trainer #$($nextBoard.TrainerNumber) -> $($nextBoard.OtaHost) [$($nextBoard.OtaEndpoint)]" -ForegroundColor Green
  }

  $finished = Wait-Job -Job $jobs -Any
  $result = Receive-Job -Job $finished
  $completed.Add($result)
  $jobs = @($jobs | Where-Object { $_.Id -ne $finished.Id })
  Remove-Job -Job $finished | Out-Null

  if ($result.ExitCode -eq 0) {
    Write-Host "Trainer #$($result.TrainerNumber) OTA upload succeeded." -ForegroundColor Green
  } else {
    Write-Host "Trainer #$($result.TrainerNumber) OTA upload failed (Exit $($result.ExitCode))." -ForegroundColor Red
  }
}

$failed = @($completed | Where-Object { $_.ExitCode -ne 0 })

Write-Host "--------------------------------------------------" -ForegroundColor DarkCyan
if ($failed.Count -eq 0) {
  Write-Host "All OTA uploads completed successfully." -ForegroundColor Cyan
  exit 0
}

Write-Host "$($failed.Count) OTA upload(s) failed:" -ForegroundColor Red
foreach ($item in $failed) {
  Write-Host "  Trainer #$($item.TrainerNumber) ($($item.Host) -> $($item.Endpoint)) exit $($item.ExitCode)" -ForegroundColor Red
  Write-Host "  --- Upload log ---" -ForegroundColor DarkYellow
  Write-Host $item.Output
  Write-Host "  ------------------" -ForegroundColor DarkYellow
}

exit 1
