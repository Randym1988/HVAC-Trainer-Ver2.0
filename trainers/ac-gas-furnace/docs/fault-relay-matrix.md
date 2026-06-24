AC-GAS FURNACE FAULT TO RELAY MATRIX

Scope:
This matrix maps furnace-relevant fault IDs to physical relay outputs and relay_* alias IDs used by instructor controls.

Columns:
- fault_id: existing fault identifier used by firmware and diagnostics
- label: expected instructor fault label in furnace mode
- physical_output: relay expander pin driven by firmware
- relay_alias: instructor relay_* alias that writes the same hardware line
- notes: caveats including NO/NC behavior and shared lines

| fault_id | label                   | physical_output | relay_alias                      | notes |
|---------:|-------------------------|-----------------|----------------------------------|-------|
| f6       | OD Fan Failure          | board_3 pin 5   | relay_condenser_fan             | NO/NC behavior applies at physical relay |
| f7       | Low Pressure Sw         | board_3 pin 6   | relay_low_pressure_switch       | NO/NC behavior applies at physical relay |
| f8       | High Pressure Sw        | board_3 pin 7   | relay_high_pressure_switch      | NO/NC behavior applies at physical relay |
| f15      | Inducer Open            | board_1 pin 0   | relay_inducer_open              | NO/NC behavior applies at physical relay |
| f16      | Press. Sw Open          | board_1 pin 4   | relay_vacuum_pressure_switch    | NO/NC behavior applies at physical relay |
| f17      | Draft Safeguard         | board_1 pin 8   | relay_draft_safeguard           | NO/NC behavior applies at physical relay |
| f17      | Draft Safeguard         | board_1 pin 8   | relay_draft_inducer_board_fault | alias used by electrical controls section |
| f18      | Igniter Open            | board_1 pin 1   | relay_igniter_open              | NO/NC behavior applies at physical relay |
| f18      | Igniter Open            | board_1 pin 1   | relay_shorted_ignition_board    | alias used by electrical controls section |
| f19      | Press. Sw Closed        | board_1 pin 5   | relay_pressure_switch_closed    | NO/NC behavior applies at physical relay |
| f20      | Gas Valve Open          | board_1 pin 9   | relay_gas_valve_open            | NO/NC behavior applies at physical relay |
| f21      | Gas Valve Closed        | board_1 pin 2   | relay_gas_valve_closed          | NO/NC behavior applies at physical relay |
| f21      | Gas Valve Closed        | board_1 pin 2   | relay_failed_gas_valve          | alias used by electrical controls section |
| f22      | Primary Limit Open      | board_1 pin 6   | relay_high_temp_limit           | NO/NC behavior applies at physical relay |
| f22      | Primary Limit Open      | board_1 pin 6   | relay_faulty_high_temp_limit    | alias used by electrical controls section |
| f23      | Blocked Flue            | board_1 pin 10  | relay_blocked_flue              | NO/NC behavior applies at physical relay |
| f24      | Indoor Fan OFF          | board_1 pin 12  | relay_indoor_fan_off            | NO/NC behavior applies at physical relay |
| f25      | Indoor Fan ON           | board_1 pin 13  | relay_indoor_fan_on             | NO/NC behavior applies at physical relay |
| f26      | Broken LPS Board        | board_3 pin 14  | relay_low_pressure_board_fault  | NO/NC behavior applies at physical relay |
| f27      | Broken HPS Board        | board_3 pin 15  | relay_high_pressure_board_fault | NO/NC behavior applies at physical relay |
| f28      | A2L Sensor              | board_1 pin 14  | relay_a2l_sensor                | NO/NC behavior applies at physical relay |
| f28      | A2L Sensor              | board_1 pin 14  | relay_faulty_a2l_sensor         | alias used by electrical controls section |
| f29      | Stuck A2L Board         | board_1 pin 15  | relay_a2l_board_fault           | NO/NC behavior applies at physical relay |
| f29      | Stuck A2L Board         | board_1 pin 15  | relay_faulty_a2l_board          | alias used by electrical controls section |
| f30      | Shorted Contactor       | board_2 pin 13  | relay_shorted_contactor         | NO/NC behavior applies at physical relay |
| f51      | Shorted Y to R          | simulation      | relay_shorted_y_to_r            | independent simulation channel |
| f54      | Shorted Contactor Coil  | simulation      | relay_shorted_contactor_coil    | independent simulation channel |
| f31      | Comp Limit Open         | board_2 pin 14  | relay_comp_limit_open           | NO/NC behavior applies at physical relay |
| f31      | Comp Limit Open         | board_2 pin 14  | relay_open_contactor_coil       | alias used by electrical controls section |
| f32      | Flame Sensor Dirty      | board_1 pin 3   | relay_flame_sensor              | NO/NC behavior applies at physical relay |
| f33      | Rollout Sw Open         | board_1 pin 7   | relay_rollout_limit_1           | NO/NC behavior applies at physical relay |
| f34      | Failed Gas Relay        | board_1 pin 11  | relay_failed_gas_relay          | shared with relay_rollout_limit_2 alias |

| f52      | Grounded W Wire         | simulation      | relay_grounded_w_wire           | independent simulation channel |
| f53      | Shorted W to R          | simulation      | relay_shorted_w_to_r            | independent simulation channel |

Unaliased furnace fault IDs:
- f2, f3, f4, f5, f9, f10, f11, f12, f13, f14 are currently exposed as direct fault toggles in instructor GUI and do not have dedicated relay_* aliases.
- These are retained as direct fault/circuit simulation IDs for clarity and compatibility.

NO/NC reminder:
- state=1 energizes relay coil: COM->NO closes, COM->NC opens.
- state=0 de-energizes relay coil: COM->NO opens, COM->NC closes.
