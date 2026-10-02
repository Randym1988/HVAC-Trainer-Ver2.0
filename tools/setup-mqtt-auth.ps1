$ErrorActionPreference = "Stop"
if ($PSVersionTable.PSVersion.Major -ge 7) {
  $PSNativeCommandUseErrorActionPreference = $false
}

$workspaceRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$composeRoot = Join-Path $workspaceRoot "platform\docker-engine"
$envPath = Join-Path $composeRoot ".env"
$brokerDataPath = Join-Path $composeRoot "mosquitto\data"
$passwordFilePath = Join-Path $brokerDataPath "passwd"
$registryPath = Join-Path $PSScriptRoot "trainer-instance-registry.json"

if (Test-Path $envPath) {
  throw "Refusing to overwrite existing local MQTT environment file: $envPath"
}
if (Test-Path $passwordFilePath) {
  throw "Refusing to overwrite existing Mosquitto password file: $passwordFilePath"
}
if (-not (Test-Path $registryPath)) {
  throw "Trainer registry not found: $registryPath"
}

$dockerCommand = Get-Command docker.exe -ErrorAction SilentlyContinue
if (-not $dockerCommand) {
  $dockerPath = Join-Path $env:LOCALAPPDATA "Programs\DockerDesktop\resources\bin\docker.exe"
  if (-not (Test-Path $dockerPath)) {
    throw "Docker CLI not found. Add Docker Desktop's resources\bin directory to PATH."
  }
} else {
  $dockerPath = $dockerCommand.Source
}

$registry = Get-Content -Path $registryPath -Raw | ConvertFrom-Json
$trainerNumbers = @(
  $registry.boards.PSObject.Properties |
    ForEach-Object { [int]$_.Value.trainerNumber } |
    Sort-Object -Unique
)
if ($trainerNumbers.Count -eq 0) {
  throw "No trainer numbers found in $registryPath"
}

function New-RandomPassword {
  $bytes = New-Object byte[] 32
  $generator = [System.Security.Cryptography.RandomNumberGenerator]::Create()
  try {
    $generator.GetBytes($bytes)
  } finally {
    $generator.Dispose()
  }
  return [System.BitConverter]::ToString($bytes).Replace("-", "").ToLowerInvariant()
}

function Invoke-MosquittoPasswordCommand {
  param(
    [Parameter(Mandatory = $true)]
    [string[]]$Arguments
  )

  $output = & $dockerPath @Arguments
  if ($LASTEXITCODE -ne 0) {
    throw "Mosquitto password-file generation failed with exit code $LASTEXITCODE."
  }
}

$enginePassword = New-RandomPassword
$trainerPasswords = @{}
foreach ($number in $trainerNumbers) {
  $trainerUsername = "trainer{0}" -f $number.ToString("00")
  $trainerPasswords[$trainerUsername] = New-RandomPassword
}

$volume = "${brokerDataPath}:/mosquitto/data"
New-Item -ItemType Directory -Path $brokerDataPath -Force | Out-Null
$passwordFileCreated = $false
$envFileCreated = $false

try {
  $arguments = @(
    "run", "--rm", "--volume", $volume, "--entrypoint", "mosquitto_passwd",
    "eclipse-mosquitto:2", "-b", "-c", "/mosquitto/data/passwd",
    "engine", $enginePassword
  )
  Invoke-MosquittoPasswordCommand -Arguments $arguments
  $passwordFileCreated = $true

  foreach ($trainerUsername in ($trainerPasswords.Keys | Sort-Object)) {
    $arguments = @(
      "run", "--rm", "--volume", $volume, "--entrypoint", "mosquitto_passwd",
      "eclipse-mosquitto:2", "-b", "/mosquitto/data/passwd",
      $trainerUsername, $trainerPasswords[$trainerUsername]
    )
    Invoke-MosquittoPasswordCommand -Arguments $arguments
  }

  $chmodArguments = @(
    "run", "--rm", "--volume", $volume, "--entrypoint", "sh",
    "eclipse-mosquitto:2", "-c", "chmod 640 /mosquitto/data/passwd"
  )
  Invoke-MosquittoPasswordCommand -Arguments $chmodArguments

  $envLines = @(
    "MQTT_ENGINE_USERNAME=engine",
    "MQTT_ENGINE_PASSWORD=$enginePassword"
  )
  foreach ($trainerUsername in ($trainerPasswords.Keys | Sort-Object)) {
    $envKey = "MQTT_{0}_PASSWORD" -f $trainerUsername.ToUpperInvariant()
    $envLines += "$envKey=$($trainerPasswords[$trainerUsername])"
  }
  Set-Content -Path $envPath -Value $envLines -Encoding ascii
  $envFileCreated = $true
} catch {
  if ($envFileCreated -and (Test-Path $envPath)) {
    Remove-Item -LiteralPath $envPath -Force
  }
  if ($passwordFileCreated -and (Test-Path $passwordFilePath)) {
    Remove-Item -LiteralPath $passwordFilePath -Force
  }
  throw
}

Write-Host "Generated an engine MQTT account and $($trainerPasswords.Count) per-trainer accounts."
Write-Host "Secrets are stored locally in $envPath and are not printed."
Write-Host "Password hashes are stored in $passwordFilePath."
Write-Host "Provision each trainer with its matching username/password before bringing it online."
