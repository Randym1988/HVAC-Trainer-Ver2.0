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
