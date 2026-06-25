#ifndef PHYSICS_ENGINE_H
#define PHYSICS_ENGINE_H

#include <Arduino.h>

class PhysicsEngine {
public:
    PhysicsEngine();
    void begin();
    void update(bool y_call, bool w_call, bool g_call, bool heat_blower_on, const bool* faults);
    void reset();

    void setAmbient(float od_temp, float id_temp, float rh);
    void setRefrigerant(String type, bool is_txv);

    float getCompAmps() const { return sim_comp_amps; }
    float getOdFanAmps() const { return sim_od_fan_amps; }
    float getIdFanAmps() const { return sim_id_fan_amps; }

    bool isLpsTripped() const { return phys_lps_tripped; }
    bool isHpsTripped() const { return phys_hps_tripped; }

    float getOdLowPress() const { return sim_od_low_press; }
    float getOdHighPress() const { return sim_od_high_press; }
    float getOdLiquidPress() const { return sim_od_liquid_press; }
    float getOdSuctionTemp() const { return sim_od_suction_temp; }
    float getOdLiquidTemp() const { return sim_od_liquid_temp; }
    float getOdDischargeTemp() const { return sim_od_discharge; }
    float getOdAmbient() const { return sim_od_ambient; }

    float getIdAmbient() const { return sim_id_ambient; }
    float getIdReturnTemp() const { return sim_id_return_temp; }
    float getIdSupplyTemp() const { return sim_id_supply_temp; }
    float getIdRh() const { return sim_id_rh; }

private:
    float add_noise(float base, float variance);

    uint32_t telemetry_timer;
    uint32_t comp_start_time;
    bool last_comp_state;

    float sim_comp_amps, sim_od_fan_amps, sim_id_fan_amps;
    bool phys_lps_tripped, phys_hps_tripped;
    String current_refrigerant;
    bool force_pressure_snap;
    bool id_is_txv;
    float set_od_temp, set_id_temp, set_rh;
    float sim_od_low_press, sim_od_high_press, sim_od_liquid_press;
    float sim_od_suction_temp, sim_od_liquid_temp, sim_od_discharge, sim_od_ambient;
    float sim_id_ambient, sim_id_return_temp, sim_id_supply_temp, sim_id_rh;
};

#endif
#ifndef PHYSICS_ENGINE_H
#define PHYSICS_ENGINE_H

#include <Arduino.h>

class PhysicsEngine {
public:
    PhysicsEngine();
    void begin();
    void update(bool y_call, bool w_call, bool g_call, bool heat_blower_on, const bool* faults);
    void reset();

    // --- Mutators / Setters ---
    void setAmbient(float od_temp, float id_temp, float rh);
    void setRefrigerant(String type, bool is_txv);

    // --- Public Getters for Telemetry ---
    float getCompAmps() const { return sim_comp_amps; }
    float getOdFanAmps() const { return sim_od_fan_amps; }
    float getIdFanAmps() const { return sim_id_fan_amps; }

    bool isLpsTripped() const { return phys_lps_tripped; }
    bool isHpsTripped() const { return phys_hps_tripped; }

    float getOdLowPress() const { return sim_od_low_press; }
    float getOdHighPress() const { return sim_od_high_press; }
    float getOdLiquidPress() const { return sim_od_liquid_press; }
    float getOdSuctionTemp() const { return sim_od_suction_temp; }
    float getOdLiquidTemp() const { return sim_od_liquid_temp; }
    float getOdDischargeTemp() const { return sim_od_discharge; }
    float getOdAmbient() const { return sim_od_ambient; }

    float getIdAmbient() const { return sim_id_ambient; }
    float getIdReturnTemp() const { return sim_id_return_temp; }
    float getIdSupplyTemp() const { return sim_id_supply_temp; }
    float getIdRh() const { return sim_id_rh; }

private:
    // --- Private Helper Methods ---
    float add_noise(float base, float variance);

    // --- Private Member Variables ---
    uint32_t telemetry_timer;
    uint32_t comp_start_time;
    bool last_comp_state;

    // Telemetry State
    float sim_comp_amps, sim_od_fan_amps, sim_id_fan_amps;
    bool phys_lps_tripped, phys_hps_tripped;
    String current_refrigerant;
    bool force_pressure_snap;
    bool id_is_txv;
    float set_od_temp, set_id_temp, set_rh;
    float sim_od_low_press, sim_od_high_press, sim_od_liquid_press;
    float sim_od_suction_temp, sim_od_liquid_temp, sim_od_discharge, sim_od_ambient;
    float sim_id_ambient, sim_id_return_temp, sim_id_supply_temp, sim_id_rh;
};

#endif
