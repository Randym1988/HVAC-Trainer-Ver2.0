# HVAC Trainer Ver2.0

Unified workspace for the Vesta HVAC trainer platform, with shared engine/frontend plus separate trainer firmware projects.

## Layout

```text
HVAC Trainer Ver2.0/
├── platform/
│   └── docker-engine/
│       ├── backend/
│       ├── frontend/
│       ├── web/
│       │   ├── instructor/
│       │   └── student/
│       └── docker-compose.yml
├── trainers/
│   ├── ac-gas-furnace/
│   │   ├── firmware/
│   │   └── docs/
│   └── heat-pump/
│       ├── firmware/
│       └── docs/
├── apps/
│   └── vesta-core-app/
├── tools/
└── docs/
```

## Quick Start

### Start Engine + Instructor/Student Portal

```bash
cd "platform/docker-engine"
docker compose up -d --build
```

### Open Trainer Firmware Projects

- AC/Gas: `trainers/ac-gas-furnace/firmware`
- Heat Pump: `trainers/heat-pump/firmware`

Each firmware folder is a standalone PlatformIO project.

## Notes

- This migration was created by copying source folders into the new layout.
- Original source folders were left intact for safety while validating the new structure.
