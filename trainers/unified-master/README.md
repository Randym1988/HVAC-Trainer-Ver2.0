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


## Compressor Maps by Refrigerant

- **R-454B:** Copeland YA31K1E-PFV (3-ton, 208/230-1-60), performance chart 99949-230 (20 F superheat, 15 F subcooling, 95 F air over, current @ 230 V, nominal +/-5%). Selecting R-454B in the instructor portal switches both the furnace and heat-pump trainers (and the engine simulation) to this map. The furnace physics uses capacity, power, amps and mass flow; the heat pump uses amps from saturated suction/discharge (dew) temperatures.
- **R-410A:** Copeland ZP29K6E-PFV chart 511570-230 on the furnace trainer; the heat-pump trainer keeps its legacy amp formula.
- **Other refrigerants:** scaled R-410A map (furnace) / legacy formula (heat pump).
- Faults 44 (internal bypass) and 45 (inefficient compressor) scale chart amps by 0.5 and 0.7/0.9 on R-454B. Inputs outside the published envelope clamp to the chart edge. Startup/locked-rotor current stays at the existing 143 A (not on the chart).
- The table lives in `firmware/src/PhysicsEngine.cpp` (`kYa31CompressorMap`) and `platform/docker-engine/backend/compressor_maps.py`; `test_compressor_maps.py` fails if they drift.

Copeland electrical component data (the selected refrigerant picks the compressor; R-454B = YA31K1E, all others = ZP29K6E):

| | YA31K1E-PFV (R-454B) | ZP29K6E-PFV (R-410A) |
|---|---|---|
| Stator | 546-5143-10 / 846-5143-10 | 546-5016-46 / 846-5016-46 (alt 546-5133-03) |
| Winding resistance (start / run, ohms +/-7%) | 1.43 / 0.72 | 1.58 / 0.92 |
| Potential relay | 040-0166-37 | 040-0166-37 |
| Start capacitor | 88-106 uF, 330 V (014-0061-27) | 88-106 uF, 330 V (014-0061-27) |
| Run capacitor | 40 uF, 370 V (014-0064-08) | 45 uF, 370 V (014-0064-25) |

Neither Copeland sheet lists RLA/LRA; take those from the unit nameplate.

### Compressor electrical readout

Status/heartbeat telemetry carries the selected compressor's ratings (`compressor_electrical_model`, `run_cap_rated_uf`/`_volts`, `start_cap_uf_low`/`_high`, `start_cap_rated_volts`, `winding_run_ohms` (C-R), `winding_start_ohms` (C-S), `potential_relay`) and live meter values derived from compressor common amps (`comp_amps`):

- `line_volts`: 241.5 V no-load, dropping 0.1 V per amp of total line current (about 227 V at locked rotor).
- `run_cap_volts`: line volts x (1.48 - 0.012 x C amps), clamped 1.25-1.5 while running; 0.6 x line at locked rotor.
- `run_cap_uf`: 98.5% of the rated run cap while running; run + mid-range start cap at locked rotor (potential relay stays closed).
- `comp_start_amps` (S) = cap volts x uF / 2652, so the field formula S amps x 2652 / cap volts gives the in-circuit uF.
- `comp_run_amps` (R): common amps is the phasor sum of R and S with S leading R by about 100 degrees.

All running values are zero (line volts excepted) while the compressor is off. The engine fills in the same values (`compressor_electrical_reading` in `backend/compressor_maps.py`) when a board's firmware does not send them; `test_compressor_maps.py` checks that both use the same constants. The student web page and the Vexera Core app show them on the Electrical tab.

## Relay Safe State

- Relays are active-low. At the very start of `setup()` (before Wi-Fi or LittleFS) every relay is released except Board 1 P0/P4/P8, which are held energized in the normal no-fault state. The PCF8575 latches outputs through a crash or reset, so this also recovers relays after a watchdog or panic reset.
- The same safe state is applied before OTA, web-update, and scheduled reboots, and by the fault reset.
- In furnace mode, the fault reset also resets `FurnaceController` so the gas valve cannot reopen without a fresh purge/ignition sequence.
- In furnace mode, `/api/toggle` returns 409 for `hs1_t1`, `hs1_t2`, `hs1_t3` and `hs2_t1` (Board 2 P0-P3 are inducer/igniter/gas valve/blower, owned by `FurnaceController`).
- Software-validated only (build). Verify on the bench with the relay checklist before field use.

## Wi-Fi Recovery (AP Mode)

If the saved Wi-Fi is not found at boot (after also trying the built-in default), or is lost for 60 seconds while running, the board starts the setup AP `Vexera Core Trainer` (LED purple) and keeps retrying the saved network every 60 seconds. Retries pause while a phone is connected to the AP.

1. Join the AP. The captive portal opens the setup page; otherwise browse to `http://192.168.4.1/wifi-setup`.
2. Pick the network from the scan list (or type it), enter the password, and tap Save & Connect. No trainer login is needed while the AP is up.
3. The network is saved only after the board joins it. A wrong password reports failure and the previous saved network is kept.
4. After joining, the AP stays up about 30 seconds so the page can show the result, then stops. Engine discovery, MQTT, mDNS and NTP restart automatically; no reboot is needed.

Outside AP mode, `/wifi-save` still requires an instructor session.

