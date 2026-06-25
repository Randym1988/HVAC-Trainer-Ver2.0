# HVAC Trainer Ver2.0

Monorepo for the Vesta HVAC trainer platform.

This workspace contains:
- ESP32-S3 firmware for furnace and heat-pump trainers
- Docker-based trainer engine backend/frontend
- web instructor/student interfaces
- supporting docs and tools

## Repository Layout

```text
HVAC Trainer Ver2.0/
|- platform/
|  |- docker-engine/
|     |- backend/
|     |- frontend/
|     |- mosquitto/
|     |- web/
|     \- docker-compose.yml
|- trainers/
|  |- ac-gas-furnace/
|  |  |- docs/
|  |  \- firmware/
|  \- heat-pump/
|     |- docs/
|     \- firmware/
|- apps/
|  \- vesta-core-app/
|- docs/
\- tools/
```

## Core Workflows

### Start Engine Stack

```bash
cd "platform/docker-engine"
docker compose up -d --build
```

Engine API: `http://localhost:8000`

### Flash Furnace Firmware

```bash
cd "trainers/ac-gas-furnace/firmware"
pio run -e usb -t upload
```

### Flash Heat-Pump Firmware

```bash
cd "trainers/heat-pump/firmware"
pio run -e usb -t upload
```

### Live Status Check

```bash
curl http://localhost:8000/api/status
```

## Current Notes

- Heat-pump thermostat call reporting is decoupled from I2C relay board presence.
- Docker engine backend uses `aiomqtt` and reconnect logic for bridge stability.
- Heat-pump backend simulation includes dedicated heating-mode pressure dynamics.
