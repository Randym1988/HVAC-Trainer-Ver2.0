# Heat-Pump Trainer Documentation

This folder now retains heat-pump hardware documentation only.

## Firmware Source

The active firmware now lives in the unified project:

```bash
cd "trainers/unified-master/firmware"
pio run -e usb
```

Use `tools/flash-unified.ps1 -Target heatpump ...` for uploads so the board keeps the correct heat-pump identity and trainer number.

## Validation Checklist

- Confirm edge registration in `GET /api/edges`
- Toggle W/Y/O/G and confirm status updates
- Confirm pressure values move in heat and cool runs
- Confirm HPS/LPS flags align with pressure and active fault states
- In cooling, apply the indoor blower fault and verify low pressure falls below
  the LPS trip threshold, the compressor stops, and the LPS remains open until
  pressure recovers above its reset threshold
- In cooling, apply the outdoor fan fault and verify both head and liquid
  pressure rise; repeat in heating and verify the outdoor-coil/low-side response
