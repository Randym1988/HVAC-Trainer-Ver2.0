#include "FurnaceController.h"
#include "main.h" // For fault_active array

FurnaceController::FurnaceController(PCF8575& b2, bool& i2c_present)
    : board_2(b2), i2c_boards_present(i2c_present) {
    // Constructor initializes references
}

void FurnaceController::begin() {
    furnace_state = FURNACE_IDLE;
    furnace_timer = 0;
    blower_on_delay = 0;
    blower_off_delay = 0;
    ignition_retry_count = 0;
    limit_trip_count = 0;
    set_relays(false, false, false, false);
}

void FurnaceController::reset() {
    begin(); // Reset to initial state
}

void FurnaceController::set_relays(bool inducer, bool igniter, bool gas, bool blower) {
    inducer_on = inducer;
    igniter_on = igniter;
    gas_valve_on = gas;
    heat_blower_on = blower;
    if (!i2c_boards_present) return;
    board_2.digitalWrite(0, inducer ? LOW : HIGH);
    board_2.digitalWrite(1, igniter ? LOW : HIGH);
    board_2.digitalWrite(2, gas ? LOW : HIGH);
    board_2.digitalWrite(3, blower ? LOW : HIGH);
}

void FurnaceController::update(bool w_call, float supply_temp) {
    uint32_t now = millis();

    if (w_call && furnace_state == FURNACE_IDLE) {
        furnace_state = FURNACE_PRE_PURGE;
        furnace_timer = now;
        ignition_retry_count = 0;
        set_relays(true, false, false, false);
    }

    switch (furnace_state) {
        case FURNACE_IDLE:
            if (blower_off_delay > 0 && now >= blower_off_delay) {
                set_relays(false, false, false, false);
                blower_off_delay = 0;
            }
            break;
        case FURNACE_PRE_PURGE:
            if (!w_call) {
                furnace_state = FURNACE_POST_PURGE;
                furnace_timer = now;
            } else if (now - furnace_timer >= 15000) {
                // Check for pressure switch stuck open (f16), blocked flue (f23), or draft inducer board fault (f17)
                bool pressure_switch_closed = !fault_active[16] && !fault_active[23] && !fault_active[17];
                // Check for open high limit (f22) or open rollout (f33)
                bool limit_ok = !fault_active[22] && !fault_active[33] && !fault_active[34];
                if (pressure_switch_closed && limit_ok) {
                    furnace_state = FURNACE_IGNITER_WARMUP;
                    furnace_timer = now;
                    set_relays(true, true, false, false);
                } else {
                    // If safeties are not met, go to lockout. Inducer remains on for safety.
                    furnace_state = FURNACE_LOCKOUT;
                }
            }
            break;
        case FURNACE_IGNITER_WARMUP:
            if (!w_call) {
                furnace_state = FURNACE_POST_PURGE;
                furnace_timer = now;
                set_relays(true, false, false, false);
            } else if (now - furnace_timer >= 10000) {
                furnace_state = FURNACE_TRIAL_FOR_IGNITION;
                furnace_timer = now;
                set_relays(true, true, true, false);
            }
            break;
        case FURNACE_TRIAL_FOR_IGNITION:
            if (!w_call) {
                furnace_state = FURNACE_POST_PURGE;
                furnace_timer = now;
                set_relays(true, false, false, false);
            } else if (now - furnace_timer >= 4000) {
                // Check for dirty flame sensor (f32), shorted ignition board (f18), or stuck-closed gas valve (f21)
                bool flame_sensed = !fault_active[32] && !fault_active[18] && !fault_active[21];

                if (flame_sensed) {
                    furnace_state = FURNACE_HEATING;
                    set_relays(true, false, true, false);
                    blower_on_delay = now + 30000;
                } else {
                    ignition_retry_count++;
                    set_relays(true, false, false, false);
                    furnace_state = (ignition_retry_count >= 3) ? FURNACE_LOCKOUT : FURNACE_PRE_PURGE;
                    furnace_timer = now;
                }
            }
            break;
        case FURNACE_HEATING:
            if (!w_call) {
                furnace_state = FURNACE_POST_PURGE;
                furnace_timer = now;
                set_relays(true, false, false, heat_blower_on);
            } else if (supply_temp > 150.0f) {
                furnace_state = FURNACE_LOCKOUT;
                set_relays(true, false, false, true);
                limit_trip_count++;
            } else if (blower_on_delay > 0 && now >= blower_on_delay) {
                set_relays(true, false, true, true);
                blower_on_delay = 0;
            }
            break;
        case FURNACE_POST_PURGE:
            if (now - furnace_timer >= 15000) {
                furnace_state = FURNACE_IDLE;
                set_relays(false, false, false, heat_blower_on);
                blower_off_delay = now + 90000;
            }
            break;
        case FURNACE_LOCKOUT:
            if (!w_call) {
                // If call for heat is removed, reset to IDLE after post-purge/cooldown.
                furnace_state = FURNACE_POST_PURGE;
                furnace_timer = now;
            } else if (supply_temp < 110.0f && limit_trip_count > 0) {
                // Auto-reset from a high-limit trip once the furnace has cooled down.
                furnace_state = FURNACE_PRE_PURGE;
                furnace_timer = now;
            }
            break;
    }
}