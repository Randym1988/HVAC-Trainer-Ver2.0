# Contributing Guide

Thank you for contributing to HVAC Next Gen.

## Branching

- Base all work on main.
- Create focused branches named by intent, for example:
  - feat/instructor-fault-timeline
  - fix/esp32-telemetry-timeout
  - docs/api-contracts

## Commit Messages

Use short, imperative commit messages.

Recommended format:

- feat: add instructor scenario timeline API
- fix: handle websocket reconnect jitter
- docs: update telemetry event schema

## Pull Requests

Each pull request should include:

1. Problem statement and scope.
2. Summary of technical changes.
3. Validation steps with commands used.
4. Risks, tradeoffs, and rollback notes.

## Code Quality Expectations

- Keep shared contracts in packages/shared-types.
- Avoid trainer-specific assumptions in shared API handlers.
- Prefer small, reviewable pull requests.
- Update docs when APIs or workflows change.

## Frontend Guidelines

- Keep UI state deterministic and avoid hidden side effects.
- Add loading, error, and empty states for all async paths.
- Keep transport logic separate from presentational components.

## API Guidelines

- Validate request payloads at service boundaries.
- Return stable, versionable response shapes.
- Log failures with enough context to diagnose field issues.

## Firmware and Hardware Communication

- Treat serial and transport links as unreliable by default.
- Add retries with bounded backoff where safe.
- Include explicit heartbeat and timeout behavior.
- Document protocol field changes before deployment.

## Local Validation Checklist

Run these before opening a PR:

```bash
pnpm install
pnpm build
pnpm test
```

For firmware-related changes, also run PlatformIO build and monitor checks in the affected trainer directory.

## Security and Safety

- Never commit secrets, tokens, or device credentials.
- Prefer environment variables and local secret stores.
- Call out safety-impacting behavior changes in PR descriptions.
