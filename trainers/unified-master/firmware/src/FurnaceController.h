#ifndef FURNACE_CONTROLLER_H
#define FURNACE_CONTROLLER_H

#include <Arduino.h>
#include "PCF8575.h"

class FurnaceController {
public:
    // --- PUBLIC INTERFACE ---
    FurnaceController(PCF8575& board2, bool& i2c_present);
    void begin();
    void update(bool w_call, float supply_temp);
    void reset();

    // --- PUBLIC STATE GETTERS ---
    bool isInducerOn() const { return inducer_on; }
    bool isIgniterOn() const { return igniter_on; }
    bool isGasValveOn() const { return gas_valve_on; }
    bool isHeatBlowerOn() const { return heat_blower_on; }
    int getFurnaceState() const { return (int)furnace_state; }

private:
    // --- PRIVATE STATE MACHINE ---
    enum FurnaceState { FURNACE_IDLE, FURNACE_PRE_PURGE, FURNACE_IGNITER_WARMUP, FURNACE_TRIAL_FOR_IGNITION, FURNACE_HEATING, FURNACE_POST_PURGE, FURNACE_LOCKOUT };
    FurnaceState furnace_state;

    // --- PRIVATE MEMBER VARIABLES ---
    PCF8575& board_2;
    bool& i2c_boards_present;
    uint32_t furnace_timer;
    uint32_t blower_on_delay;
    uint32_t blower_off_delay;
    int ignition_retry_count;
    int limit_trip_count;
    bool inducer_on, igniter_on, gas_valve_on, heat_blower_on;

    // --- PRIVATE HELPER METHODS ---
    void set_relays(bool inducer, bool igniter, bool gas, bool blower);
};

#endif