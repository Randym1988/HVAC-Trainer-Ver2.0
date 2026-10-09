#ifndef PHYSICS_ENGINE_H
#define PHYSICS_ENGINE_H

#include <Arduino.h>

struct CompressorOperatingPoint {
    float capacity_btu_per_hour;
    float power_watts;
    float current_amps;
    float mass_flow_lb_per_hour;
};

// Copeland electrical component data for a compressor (PSC with potential-relay start kit).
struct CompressorElectricalSpec {
    const char* model;
    float run_cap_uf;
    float run_cap_volts;
    float start_cap_uf_low;
    float start_cap_uf_high;
    float start_cap_volts;
    float start_winding_ohms;   // C-S
    float run_winding_ohms;     // C-R
    const char* potential_relay;
};

// What a meter would read on the running compressor circuit.
struct CompressorElectricalReading {
    float line_volts;           // L1-L2 at the contactor
    float run_cap_volts;        // across the run capacitor (HERM-C)
    float start_winding_amps;   // S lead (through the run capacitor)
    float run_winding_amps;     // R lead
    float run_cap_uf;           // in-circuit capacitance: start amps x 2652 / cap volts
};

class PhysicsEngine {
public:
    PhysicsEngine();
    void begin();
    void update(bool y_call, bool w_call, bool g_call, bool physical_blower_on, const bool* faults);
    void reset();

    void setAmbient(float od_temp, float id_temp, float rh);
    void setRefrigerant(String type, bool is_txv);

    static float evaluateCapacityMap(float suction_temp_f, float discharge_temp_f);
    static float evaluatePowerMap(float suction_temp_f, float discharge_temp_f);
    static float evaluateCurrentMap(float suction_temp_f, float discharge_temp_f);
    static float evaluateMassFlowMap(float suction_temp_f, float discharge_temp_f);
    // Evaluates the compressor linked to the refrigerant (YA31K1E for R454B, ZP29K6E otherwise).
    // Returns true when the chart was published for that refrigerant (no scaling needed).
    static bool evaluateCompressorForRefrigerant(const String& refrigerant,
        float suction_temp_f, float discharge_temp_f, CompressorOperatingPoint& out);
    // Model name when the heat-pump trainer shares this refrigerant's compressor map, else nullptr.
    static const char* heatPumpCompressorModelName(const String& refrigerant);
    // Electrical data for the compressor linked to the refrigerant (YA31K1E for R454B, ZP29K6E otherwise).
    static const CompressorElectricalSpec& compressorElectricalSpec(const String& refrigerant);
    // Running meter readings derived from compressor (common) amps and total line amps.
    static CompressorElectricalReading compressorElectricalReading(const String& refrigerant,
        float comp_amps, float total_line_amps);

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
    float getSatSuctionTemp() const { return sat_suction_temp; }
    float getSatDischargeTemp() const { return sat_discharge_temp; }
    float getSuperheat() const { return sim_od_suction_temp - sat_suction_temp; }
    float getSubcooling() const { return sat_discharge_temp - sim_od_liquid_temp; }
    float getOdAmbient() const { return sim_od_ambient; }

    float getIdAmbient() const { return sim_id_ambient; }
    float getIdReturnTemp() const { return sim_id_return_temp; }
    float getIdSupplyTemp() const { return sim_id_supply_temp; }
    float getIdRh() const { return sim_id_rh; }
    
    // New getters for additional telemetry
    float getSimulatedCfm() const { return simulated_cfm; }
    float getStaticPressure() const { return static_pressure; }
    float getCompressorCapacityBtuPerHour() const { return compressor_capacity_btu_per_hour; }
    float getCompressorMassFlowLbPerHour() const { return compressor_mass_flow_lb_per_hour; }
    float getCompressorPowerWatts() const { return compressor_power_watts; }
    const char* getCompressorModelName() const;
    const char* getTelemetryState() const { return telemetry_state.c_str(); }
    bool isHighLimitTripped() const { return high_limit_tripped; }
    bool isFlameActive() const { return flame_active; }
    bool isBlowerRunning() const { return blower_running; }

private:
    float add_noise(float base, float variance);
    float pressureToTemp(float psi, String refrigerant) const;
    float tempToPressure(float temp_f, String refrigerant) const;
    float tempToPressure(float temp_f, String refrigerant, bool dew_point) const;

    static const float kElectricalTimeConstantSeconds;
    static const float kPressureTimeConstantSeconds;
    static const float kThermalMassTimeConstantSeconds;
    static const float kEqualizationTimeConstantSeconds;

    uint32_t telemetry_timer;
    uint32_t comp_start_time;
    uint32_t last_update_time;
    bool last_comp_state;

    float sim_comp_amps, sim_od_fan_amps, sim_id_fan_amps;
    float sim_od_low_press, sim_od_high_press, sim_od_liquid_press;
    float sim_od_suction_temp, sim_od_liquid_temp, sim_od_discharge, sim_od_ambient;
    float sim_id_ambient, sim_id_return_temp, sim_id_supply_temp, sim_id_rh;

    float current_comp_amps;
    float current_od_fan_amps;
    float current_id_fan_amps;
    float current_low_pressure;
    float current_high_pressure;
    float current_liquid_pressure;
    float current_evap_temp_f;
    float current_cond_temp_f;
    float sat_suction_temp;
    float sat_discharge_temp;
    float current_id_return_temp_f;
    float current_id_supply_temp_f;
    float current_id_rh;
    float compressor_capacity_btu_per_hour;
    float compressor_mass_flow_lb_per_hour;
    float compressor_power_watts;

    bool phys_lps_tripped, phys_hps_tripped;
    String current_refrigerant;
    bool id_is_txv;
    float set_od_temp, set_id_temp, set_rh;

    bool flame_active;
    bool blower_running;
    bool high_limit_tripped;
    uint32_t gas_valve_on_time;
    uint32_t ignition_timer;
    float simulated_cfm;
    float static_pressure;
    String telemetry_state;
};

#endif
