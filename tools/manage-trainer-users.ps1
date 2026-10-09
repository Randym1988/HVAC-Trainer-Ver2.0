param(
  [Parameter(Mandatory = $true)]
  [ValidateSet("List", "Export", "Delete", "ResetPassword")]
  [string]$Action,

  [ValidateSet("Engine", "Furnace", "HeatPump", "All")]
  [string]$Target = "All",

  # Account to delete or reset (Delete / ResetPassword only; needs a single -Target).
  [string]$User,

  [string]$EngineUrl = "http://localhost:8000",
  [string]$FurnaceHost = "192.168.1.151",
  [string]$HeatPumpHost = "192.168.1.150",

  # Export destination. Contains salted password hashes; it is git-ignored.
  [string]$OutFile = (Join-Path $PSScriptRoot "user-registry.json")
)

# Passwords are stored only as salted PBKDF2 hashes, so they cannot be read back.
# "Recovering" a forgotten password means resetting it (-Action ResetPassword).

$ErrorActionPreference = "Stop"
$engineDir = Join-Path $PSScriptRoot "..\platform\docker-engine"

function Get-BaseUrl([string]$name) {
  switch ($name) {
    "Engine"   { return $EngineUrl.TrimEnd("/") }
    "Furnace"  { return "http://$FurnaceHost" }
    "HeatPump" { return "http://$HeatPumpHost" }
  }
}

function Invoke-Api([string]$base, [string]$token, [string]$method, [string]$path, [hashtable]$form) {
  $params = @{
    Uri             = "$base$path"
    Method          = $method
    Headers         = @{ Authorization = "Bearer $token" }
    UseBasicParsing = $true
    TimeoutSec      = 30
  }
  if ($form) { $params.Body = $form }
  for ($attempt = 1; $attempt -le 3; $attempt++) {
    try { return Invoke-WebRequest @params } catch {
      if ($attempt -eq 3) { throw }
      Start-Sleep -Seconds 2
    }
  }
}

function Connect-Target([string]$name) {
  $base = Get-BaseUrl $name
  $cred = Get-Credential -Message "Instructor login for $name ($base)"
  if (-not $cred) { throw "Login cancelled." }
  $plain = $cred.GetNetworkCredential().Password
  $response = $null
  for ($attempt = 1; $attempt -le 3 -and -not $response; $attempt++) {
    try {
      $response = Invoke-WebRequest -Uri "$base/api/login" -Method Post -UseBasicParsing -TimeoutSec 30 `
        -Body @{ user = $cred.UserName; pass = $plain }
    } catch {
      if ($attempt -eq 3) { throw "Login to $name failed: $($_.Exception.Message)" }
      Start-Sleep -Seconds 2
    }
  }
  return @{ Base = $base; Token = ($response.Content | ConvertFrom-Json).token }
}

function Get-UserList([string]$name, $session, [bool]$withHashes) {
  if ($name -eq "Engine" -and $withHashes) {
    # The engine only exposes hash fingerprints over HTTP; read full hashes from its data volume.
    Push-Location $engineDir
    try {
      docker compose exec -T engine python manage_users.py export-users /tmp/users-export.json | Out-Null
      $json = docker compose exec -T engine cat /tmp/users-export.json
      docker compose exec -T engine rm -f /tmp/users-export.json | Out-Null
    } finally { Pop-Location }
    $records = ($json -join "`n") | ConvertFrom-Json
    return $records.PSObject.Properties | ForEach-Object {
      [pscustomobject]@{ user = $_.Name; role = $_.Value.role; pw_hash = $_.Value.pw_hash }
    }
  }
  $path = "/api/users"
  if ($withHashes) { $path = "/api/users?hashes=1" }
  $response = Invoke-Api $session.Base $session.Token "Get" $path $null
  return ($response.Content | ConvertFrom-Json).users
}

$targets = @("Engine", "Furnace", "HeatPump")
if ($Target -ne "All") { $targets = @($Target) }

if ($Action -in @("Delete", "ResetPassword")) {
  if ($targets.Count -ne 1) { throw "-Action $Action needs a single -Target (Engine, Furnace or HeatPump)." }
  if (-not $User) { throw "-User is required for $Action." }
}

$registry = [ordered]@{}
foreach ($name in $targets) {
  $session = Connect-Target $name
  switch ($Action) {
    "List" {
      Write-Host "`n== $name =="
      Get-UserList $name $session $false |
        Select-Object user, role, hash_scheme, hash_fingerprint | Format-Table -AutoSize
    }
    "Export" {
      $registry[$name] = @(Get-UserList $name $session $true)
      Write-Host "$name : $($registry[$name].Count) accounts"
    }
    "Delete" {
      $answer = Read-Host "Delete '$User' from $name? Type YES to confirm"
      if ($answer -ne "YES") { throw "Cancelled." }
      Invoke-Api $session.Base $session.Token "Post" "/api/users/delete" @{ user = $User } | Out-Null
      Write-Host "Deleted '$User' from $name."
    }
    "ResetPassword" {
      $new = Read-Host "New password for '$User' on $name" -AsSecureString
      $plain = [System.Net.NetworkCredential]::new("", $new).Password
      if ($plain.Length -lt 3) { throw "Password must be at least 3 characters." }
      Invoke-Api $session.Base $session.Token "Post" "/api/users/reset-password" @{ user = $User; pass = $plain } | Out-Null
      Write-Host "Password for '$User' on $name was reset."
    }
  }
}

if ($Action -eq "Export") {
  $registry | ConvertTo-Json -Depth 6 | Set-Content -Path $OutFile -Encoding UTF8
  Write-Host "Registry written to $OutFile (contains password hashes; keep private)."
}
