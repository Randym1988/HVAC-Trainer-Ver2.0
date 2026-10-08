# Furnace Trainer Documentation

This folder now retains AC/gas furnace hardware documentation only.

## Firmware Source

The active firmware now lives in the unified project:

```bash
cd "trainers/unified-master/firmware"
pio run -e usb
```

Use `tools/flash-unified.ps1 -Target furnace ...` for uploads so the board keeps the correct furnace identity and trainer number.

## Runtime Validation

After flashing:
- verify trainer appears in `GET /api/edges`
- verify calls and pressure flags in `GET /api/status`
- verify no ESP32 reboot/assert loop in serial monitor
- In cooling, apply the indoor blower fault and verify low pressure drops below
  the LPS trip threshold, then stays tripped until pressure recovers above the
  reset threshold
- In cooling, apply the condenser-fan fault and verify head and liquid pressure
  rise while the compressor is running
