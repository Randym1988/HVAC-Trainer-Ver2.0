param(
  [Parameter(Mandatory = $true)]
  [string]$TrainerHost,

  [Parameter(Mandatory = $true)]
  [ValidateRange(1, 999)]
  [int]$TrainerNumber
)

$ErrorActionPreference = "Stop"

$workspaceRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$envPath = Join-Path $workspaceRoot "platform\docker-engine\.env"
$username = "trainer{0}" -f $TrainerNumber.ToString("00")
$envKey = "MQTT_{0}_PASSWORD" -f $username.ToUpperInvariant()

if (-not (Test-Path $envPath)) { throw "Missing $envPath" }
$line = Get-Content $envPath | Where-Object { $_ -like "$envKey=*" } | Select-Object -First 1
if (-not $line) { throw "$envKey not found in $envPath" }
$mqttPassword = ($line -replace '^[^=]+=', '').Trim()

# Login dialog for the trainer's instructor account.
$cred = Get-Credential -Message "Instructor login for $TrainerHost"
if (-not $cred) { throw "Login cancelled." }

$base = "http://$TrainerHost"
$loginBody = @{
  user = $cred.UserName
  pass = $cred.GetNetworkCredential().Password
}
$login = Invoke-RestMethod -Method Post -Uri "$base/api/login" -Body $loginBody
if ($login.role -ne "admin" -and $login.role -ne "instructor") {
  throw "Account role '$($login.role)' cannot provision MQTT credentials."
}

$result = Invoke-RestMethod -Method Post -Uri "$base/api/mqtt/credentials" `
  -Headers @{ Authorization = "Bearer $($login.token)" } `
  -Body @{ username = $username; password = $mqttPassword }
Write-Host $result
Write-Host "Provisioned $username on $TrainerHost. Check the broker log for a successful connect."
