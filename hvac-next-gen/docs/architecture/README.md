# Architecture Notes

## Domains

- Instructor Console: scenario orchestration and grading controls.
- Student Interface: guided labs and telemetry-driven diagnostics.
- Control API: simulation state, telemetry distribution, and fault orchestration.
- Firmware: hardware abstraction and relay/sensor control per trainer model.

## Integration Direction

- Firmware publishes telemetry to API (MQTT/WebSocket bridge as next step).
- API normalizes telemetry into shared contracts from `packages/shared-types`.
- Web and mobile clients consume live updates from API.
