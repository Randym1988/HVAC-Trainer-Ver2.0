AC-GAS FURNACE RELAY VALIDATION CHECKLIST

Purpose:
Validate relay alias IDs end-to-end from instructor controls through firmware output and physical relay NO/NC contact behavior.

Pre-checks:
1. Deploy latest docker engine/frontend.
2. Flash latest furnace firmware.
3. Ensure trainer type is furnace in instructor UI.
4. Prepare meter/probe access to relay COM/NO/NC terminals.

State conventions:
- Command state=1: coil energized, COM->NO closed, COM->NC open.
- Command state=0: coil de-energized, COM->NO open, COM->NC closed.

Quick API test pattern:
- POST /api/toggle?id=<relay_id>&state=1
- Confirm UI toggle active and expected telemetry key = 1.
- Verify physical NO/NC behavior at relay terminals.
- POST /api/toggle?id=<relay_id>&state=0
- Confirm UI toggle inactive and telemetry key = 0.
- Verify physical NO/NC behavior returned to de-energized state.

Core relay outputs:
- relay_inducer
- relay_igniter
- relay_gas_valve
- relay_heat_blower
- relay_compressor_motor
- relay_compressor_contactor
- relay_condenser_y
- relay_y_wire_interlock

Furnace fault-line alias relays:
- relay_rollout_limit_1 (f33)
- relay_rollout_limit_2 (f34)
- relay_high_temp_limit (f22)
- relay_flame_sensor (f32)
- relay_condenser_fan (f6)
- relay_low_pressure_switch (f7)
- relay_high_pressure_switch (f8)
- relay_vacuum_pressure_switch (f16)
- relay_pressure_switch_closed (f19)
- relay_inducer_open (f15)
- relay_igniter_open (f18)
- relay_gas_valve_closed (f21)
- relay_gas_valve_open (f20)
- relay_draft_safeguard (f17)
- relay_blocked_flue (f23)
- relay_indoor_fan_off (f24)
- relay_indoor_fan_on (f25)
- relay_shorted_contactor (f30)
- relay_comp_limit_open (f31)
- relay_low_pressure_board_fault (f26)
- relay_high_pressure_board_fault (f27)
- relay_a2l_sensor (f28)
- relay_a2l_board_fault (f29)
- relay_failed_gas_relay (f34)

Pass criteria:
1. API toggle request returns success.
2. Instructor relay button and websocket telemetry state match commanded state.
3. Physical relay coil and NO/NC contacts behave as expected.
4. Any mapped f* side effects are expected and documented.

Troubleshooting:
- If UI changes but physical relay does not: verify firmware flashed and board I2C expander path is healthy.
- If physical relay toggles but UI does not: verify websocket status stream and telemetry key naming.
- If behavior is inverted in field circuit: check whether load is wired to NC instead of NO.
