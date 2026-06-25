# Furnace Trainer Firmware

This folder contains the AC/Gas furnace trainer firmware (ESP32-S3, PlatformIO).

## Scope

- Furnace state machine and timing
- Physics and telemetry generation
- BLE + web API communications
- Engine heartbeat/status sync

## Key Source Files

- `src/main.cpp` - startup, scheduling, shared globals
- `src/FurnaceController.cpp` - furnace sequence/state logic
- `src/PhysicsEngine.cpp` - pressure/temperature model
- `src/CommManager.cpp` - BLE/web communication layer

## Build and Flash

```bash
cd "trainers/ac-gas-furnace/firmware"
pio run -e usb
pio run -e usb -t upload
```

## Runtime Validation

After flashing:
- verify trainer appears in `GET /api/edges`
- verify calls and pressure flags in `GET /api/status`
- verify no ESP32 reboot/assert loop in serial monitor
