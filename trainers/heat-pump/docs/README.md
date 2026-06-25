# Heat-Pump Trainer Firmware

This folder contains the heat-pump trainer firmware (ESP32-S3, PlatformIO).

## Scope

- Heat-pump control and thermostat call handling
- Refrigerant and electrical telemetry simulation
- fault/simulation injection logic
- BLE + web + engine heartbeat integration

## Current Runtime Notes

- Thermostat call polling runs even when I2C relay boards are not detected.
- Control and comm loops support dual-core split with single-loop fallback.
- Heating mode pressure behavior is expected to track ambient changes when compressor is running.

## Build and Flash

```bash
cd "trainers/heat-pump/firmware"
pio run -e usb
pio run -e usb -t upload
```

## Validation Checklist

- Confirm edge registration in `GET /api/edges`
- Toggle W/Y/O/G and confirm status updates
- Confirm pressure values move in heat and cool runs
- Confirm HPS/LPS flags align with pressure and active fault states
