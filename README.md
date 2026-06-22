# HVAC Next Gen Monorepo

Purpose-built monorepo for large HVAC trainer systems combining web/mobile apps, control APIs, and embedded firmware.

## Stack

- Monorepo: Turborepo + pnpm workspaces
- Web apps: React + Vite + TypeScript
- API: Node.js + Fastify + TypeScript
- Embedded: PlatformIO (ESP32) per trainer
- Deployment: Docker Compose
- CI: GitHub Actions

## Repository Layout

```text
hvac-next-gen/
  apps/
    instructor-web/
    student-web/
    mobile-core/
  services/
    control-api/
  packages/
    shared-types/
  trainers/
    ac-gas-furnace/firmware/
    heat-pump/firmware/
  platform/docker/
  docs/architecture/
```

## Quick Start

1. Install Node.js 20+ and pnpm 9+.
2. Install dependencies:

```bash
pnpm install
```

3. Start all JavaScript/TypeScript dev targets:

```bash
pnpm dev
```

4. Build everything:

```bash
pnpm build
```

5. Start local platform stack:

```bash
cd platform/docker
docker compose up -d --build
```

## Notes

- `apps/mobile-core` is reserved for the Capacitor shell and mobile runtime integration.
- Firmware folders are independent PlatformIO projects.
- `packages/shared-types` stores protocol models reused by frontend and API.
