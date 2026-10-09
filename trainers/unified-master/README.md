# Unified ESP32-S3 Trainer Firmware

This project is the single master firmware build for both trainer hardware profiles.

## Hardware Identity Select

- GPIO14 is configured as INPUT_PULLUP at boot.
- GPIO14 LOW (strapped to GND): `STRAIGHT_AC_FURNACE` mode
- GPIO14 HIGH (open/floating): `HEAT_PUMP` mode
- GPIO14 is latched at boot. Changing the strap while running does not change trainer mode until reboot.

## Build

```bash
cd "trainers/unified-master/firmware"
pio run -e usb
```

## Flash

Do not use raw `pio ... -t upload` for field boards. Trainer identity is stamped at flash time, so uploads should go through the guarded scripts below.

## Safe Flash Workflow (Recommended)

Use the guarded script in the workspace root to avoid flashing the wrong board by accident.

```powershell
Set-Location "e:\Randy\HVAC Trainer Ver2.0"
.\tools\flash-unified.ps1 -Target furnace -Port COM9
```

```powershell
Set-Location "e:\Randy\HVAC Trainer Ver2.0"
.\tools\flash-unified.ps1 -Target heatpump -Port COM7
```

Optional explicit numbering override:

```powershell
Set-Location "e:\Randy\HVAC Trainer Ver2.0"
.\tools\flash-unified.ps1 -Target heatpump -Port COM7 -TrainerNumber 2
```

What this guard does:

- Forces explicit target selection (`furnace` or `heatpump`)
- Reads the connected ESP32 MAC before upload and compares with expected board MAC
- Blocks upload on mismatch unless `-Force` is explicitly passed
- Reminds required GPIO14 strap for the selected profile
- Assigns a trainer number per board MAC and stores it in `tools/trainer-instance-registry.json`
- Stamps the firmware label/edge identity with that trainer number (for example, `Heat Pump Trainer 02`)

## OTA Updates (No USB After Initial Provisioning)

After each board has been flashed once by USB with the guard script, you can update over Wi-Fi by trainer number:

```powershell
Set-Location "e:\Randy\HVAC Trainer Ver2.0"
.\tools\flash-unified-ota.ps1 -TrainerNumber 1
```

```powershell
Set-Location "e:\Randy\HVAC Trainer Ver2.0"
.\tools\flash-unified-ota.ps1 -TrainerNumber 2
```

To update all registered trainers concurrently in one command:

```powershell
Set-Location "e:\Randy\HVAC Trainer Ver2.0"
.\tools\flash-all-ota.ps1
```

Notes:

- OTA hostnames are now unique per trainer number (`trainer01.local`, `trainer02.local`, ...).
- Registry file `tools/trainer-instance-registry.json` maps board MAC -> trainer number -> OTA host.
- If multiple boards share a target profile, use `-TrainerNumber` to avoid ambiguity.
- Fleet OTA builds trainer-specific artifacts first, then pushes OTA uploads in parallel.

## Architecture

- Shared core services in all modes: Wi-Fi, BLE, OTA, web API, websocket telemetry, and engine heartbeat.
- Mode-specific control engine selected at startup from GPIO14.
  - `HEAT_PUMP`: existing heat pump simulation/control logic.
  - `STRAIGHT_AC_FURNACE`: furnace controller + physics engine, including gas valve and blower monitor inputs via optocouplers.

## Instructor Simulations

The portal exposes simulations 1-15. Simulations 1-4, 6, and 15 are shared
between trainer profiles; furnace mode maps blower failures (1, 2, 6) to f24,
outdoor fan failures (3, 4) to f6, and compressor overload (15) to f31.
Simulations 5 and 7-14 are heat-pump-only and are hidden in furnace mode.

Simulation 13 is the intermittent reversing-valve coil fault. It uses the
same Board 3 pin 11 fault channel as simulation 5, but cycles the fault while
the O/B call is active. Simulation 14 models an intermittent Y-wire break.
The backend mirrors reversing-valve, O/B mode, and expected-diagnosis behavior
for these simulations. Bench validation of relay contacts and trainer behavior
is still required; software checks do not establish physical operation.

## API Access

- Sign in with `POST /api/login` using form fields `user` and `pass`; credentials are no longer accepted in the URL.
- The response includes a short-lived bearer token. Send it as `Authorization: Bearer <token>` for trainer API writes.
- Status, identity, and engine discovery reads remain public. Trainer controls and user management require an instructor role; diagnosis submission remains available to student and guest training flows.
- Browser firmware updates require HTTP Digest authentication. The OTA password is still shared by the firmware and update script, so use the trainer only on a trusted network until that credential is moved to per-device provisioning.

## MQTT Credentials

The local broker requires authentication. Generate the local engine and per-trainer credentials before starting the stack:

```powershell
Set-Location "e:\Randy\HVAC Trainer Ver2.0"
.\tools\setup-mqtt-auth.ps1
```

The script stores secrets locally in the ignored `platform/docker-engine/.env` and writes only password hashes to Mosquitto’s ignored data directory. Do not commit either file.

The firmware stores optional MQTT credentials in NVS. After flashing the credential-capable firmware, log in to each trainer as an instructor and provision its matching account with `POST /api/mqtt/credentials` using form fields `username` and `password`. The username must match the assigned trainer ID (for example, `trainer01`), and the password must be 20-128 characters. Use the matching `MQTT_TRAINERxx_PASSWORD` value from the local `.env` without pasting it into chat.

The trainer provisioning endpoint currently uses HTTP. Provision only on a trusted local network until TLS is configured for trainer web access. Do not bring a trainer back online against the authenticated broker until its matching credentials have been provisioned.

## User Accounts

Trainer boards and the engine store passwords only as salted PBKDF2-SHA256 hashes (`pbkdf2_sha256$rounds$salt$digest`), so a forgotten password cannot be read back; it is reset instead. Existing plaintext records on a board are hashed automatically at boot.

Use `tools\manage-trainer-users.ps1` (instructor login required, prompted per target):

- `-Action List` shows usernames, roles and hash fingerprints for `-Target Engine|Furnace|HeatPump|All`.
- `-Action Export` writes every account and its full hash to `tools\user-registry.json` (git-ignored; keep private).
- `-Action ResetPassword -Target <t> -User <name>` sets a new password.
- `-Action Delete -Target <t> -User <name>` removes an account. You cannot delete yourself or the last administrator.

Board endpoints: `GET /api/users` (add `?hashes=1` for full hashes), `POST /api/users/delete`, `POST /api/users/reset-password`. The engine has the same routes, an account list in the instructor portal, and `manage_users.py list-users|export-users` for host-side access.

## Server Auto-Discovery

The Docker engine answers UDP broadcasts on port 4210. A packet containing `DISCOVER_HVAC_TRAINER` gets a JSON reply (`service`, `version`, `http_port`, `mqtt_port`, `hostname`). The mobile app broadcasts this at launch for 3 seconds: one reply connects automatically, several show a selection dialog, none shows a manual-address dialog with Retry Discovery. Set `DISCOVERY_HOSTNAME` in the engine environment to change the advertised name. On a Linux Docker host, published ports may not receive LAN broadcasts; use `network_mode: host` for the engine there.


## Wi-Fi Recovery (AP Mode)

If the saved Wi-Fi is not found at boot (after also trying the built-in default), or is lost for 60 seconds while running, the board starts the setup AP `Vexera Core Trainer` (LED purple) and keeps retrying the saved network every 60 seconds. Retries pause while a phone is connected to the AP.

1. Join the AP. The captive portal opens the setup page; otherwise browse to `http://192.168.4.1/wifi-setup`.
2. Pick the network from the scan list (or type it), enter the password, and tap Save & Connect. No trainer login is needed while the AP is up.
3. The network is saved only after the board joins it. A wrong password reports failure and the previous saved network is kept.
4. After joining, the AP stays up about 30 seconds so the page can show the result, then stops. Engine discovery, MQTT, mDNS and NTP restart automatically; no reboot is needed.

Outside AP mode, `/wifi-save` still requires an instructor session.

