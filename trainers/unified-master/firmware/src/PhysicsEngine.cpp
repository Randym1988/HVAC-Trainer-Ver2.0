#include "PhysicsEngine.h"

PhysicsEngine::PhysicsEngine()
	: telemetry_timer(0),
	  comp_start_time(0),
	  last_comp_state(false),
	  sim_comp_amps(0.0f),
	  sim_od_fan_amps(0.0f),
	  sim_id_fan_amps(0.0f),
	  phys_lps_tripped(false),
	  phys_hps_tripped(false),
	  current_refrigerant("R410A"),
	  force_pressure_snap(true),
	  id_is_txv(true),
	  set_od_temp(95.0f),
	  set_id_temp(75.0f),
	  set_rh(50.0f),
	  sim_od_low_press(125.0f),
	  sim_od_high_press(320.0f),
	  sim_od_liquid_press(305.0f),
	  sim_od_suction_temp(52.0f),
	  sim_od_liquid_temp(98.0f),
	  sim_od_discharge(150.0f),
	  sim_od_ambient(95.0f),
	  sim_id_ambient(75.0f),
	  sim_id_return_temp(75.0f),
	  sim_id_supply_temp(58.0f),
	  sim_id_rh(50.0f),
      // Initialize new variables
      flame_active(false),
      blower_running(false),
      high_limit_tripped(false),
      gas_valve_on_time(0),
      ignition_timer(0),
      simulated_cfm(0.0f),
      static_pressure(0.0f),
      telemetry_state("Idle") {}

void PhysicsEngine::begin() {
	telemetry_timer = millis();
	reset();
}

void PhysicsEngine::reset() {
	phys_lps_tripped = false;
	phys_hps_tripped = false;
	force_pressure_snap = true;
	last_comp_state = false;
	comp_start_time = 0;
    flame_active = false;
    blower_running = false;
    high_limit_tripped = false;
    gas_valve_on_time = 0;
    ignition_timer = 0;
    simulated_cfm = 0.0f;
    static_pressure = 0.0f;
    telemetry_state = "Idle";


	sim_comp_amps = 0.0f;
	sim_od_fan_amps = 0.0f;
	sim_id_fan_amps = 0.0f;

	sim_od_ambient = set_od_temp;
	sim_id_ambient = set_id_temp;
	sim_id_return_temp = set_id_temp;
	sim_id_supply_temp = set_id_temp - 2.0f;
	sim_id_rh = set_rh;

	sim_od_low_press = 125.0f;
	sim_od_high_press = 320.0f;
	sim_od_liquid_press = 305.0f;
	sim_od_suction_temp = sim_id_supply_temp + 6.0f;
	sim_od_liquid_temp = sim_od_ambient + 5.0f;
	sim_od_discharge = sim_od_ambient + 55.0f;
}

void PhysicsEngine::setAmbient(float od_temp, float id_temp, float rh) {
	set_od_temp = od_temp;
	set_id_temp = id_temp;
	set_rh = rh;
}

void PhysicsEngine::setRefrigerant(String type, bool is_txv) {
	current_refrigerant = type;
	id_is_txv = is_txv;
	force_pressure_snap = true;
}

float PhysicsEngine::add_noise(float base, float variance) {
	float rnd = (float)(esp_random() % 1000) / 999.0f;
	float centered = (rnd * 2.0f) - 1.0f;
	return base + (centered * variance);
}

void PhysicsEngine::update(bool y_call, bool w_call, bool g_call, bool physical_blower_on, const bool* faults) {
	bool has_faults = faults != nullptr;
	bool compressor_failed = has_faults && (faults[4] || faults[31]);
	bool cooling_call = y_call && !compressor_failed;
	bool indoor_fan_failed = has_faults && faults[24];
	bool outdoor_fan_failed = has_faults && faults[6];	
    
    // The physical blower input is now monitored from the specified GPIO pin
    blower_running = physical_blower_on && !indoor_fan_failed;
    // The `w_call` is our "Gas Valve Input" for the purpose of this simulation
    bool gas_valve_active = w_call;
    uint32_t now = millis();

    // 1. Flame/Gas Monitoring
    if (gas_valve_active && gas_valve_on_time == 0) {
        gas_valve_on_time = now;
        ignition_timer = now + 2000; // 2-second ignition delay
        telemetry_state = "Ignition Delay";
    } else if (!gas_valve_active) {
        gas_valve_on_time = 0;
        ignition_timer = 0;
        flame_active = false;
        telemetry_state = "Idle";
    }

    if (ignition_timer > 0 && now >= ignition_timer) {
        flame_active = true;
        telemetry_state = "Burners Lit";
        ignition_timer = 0; // Prevent re-triggering
    }

    // 2. Blower Monitoring
    if (blower_running) {
        simulated_cfm = 1200.0f;
        static_pressure = 0.5f;
    } else {
        simulated_cfm = 0.0f;
        static_pressure = 0.0f;
    }

    // 3. Safety/Fault Simulation
    if (gas_valve_active && now - gas_valve_on_time > 90000 && !blower_running) {
        high_limit_tripped = true;
        telemetry_state = "High Limit Tripped";
    }

	sim_od_ambient = set_od_temp; // Keep AC side simulation logic
	sim_id_ambient = set_id_temp;
	sim_id_return_temp = add_noise(set_id_temp, 0.2f);
	sim_id_rh = set_rh;

	sim_id_fan_amps = blower_running ? 3.8f : 0.0f;
	sim_od_fan_amps = cooling_call && !outdoor_fan_failed ? 0.9f : 0.0f;

	if (cooling_call && !last_comp_state) {
		comp_start_time = millis();
	}
	last_comp_state = cooling_call;

	if (cooling_call) {
		float lp_base = current_refrigerant == "R22" ? 68.0f : 122.0f;
		float hp_base = current_refrigerant == "R22" ? 245.0f : 340.0f;

		if (!id_is_txv) {
			lp_base -= 8.0f;
			hp_base += 10.0f;
		}

		if (has_faults && faults[46]) {
			lp_base -= 20.0f;
			hp_base += 35.0f;
		}
		if (has_faults && faults[47]) {
			lp_base += 20.0f;
			hp_base -= 30.0f;
		}
		if (has_faults && faults[40]) {
			hp_base += 70.0f;
		}
		if (has_faults && faults[41]) {
			lp_base += 25.0f;
			hp_base += 20.0f;
		}
		if (has_faults && faults[42]) {
			lp_base -= 35.0f;
			hp_base -= 10.0f;
		}
		if (has_faults && faults[43]) {
			lp_base -= 15.0f;
			hp_base -= 5.0f;
		}
		if (has_faults && faults[44]) {
			lp_base += 18.0f;
			hp_base -= 25.0f;
		}
		if (has_faults && faults[45]) {
			lp_base += 10.0f;
			hp_base -= 15.0f;
		}

		if (outdoor_fan_failed) {
			hp_base += 120.0f;
		}

		sim_od_low_press = add_noise(lp_base, 1.0f);
		sim_od_high_press = add_noise(hp_base, 2.0f);
		sim_od_liquid_press = sim_od_high_press - 12.0f;

		sim_od_suction_temp = add_noise((set_id_temp - 18.0f), 0.4f);
		sim_od_liquid_temp = add_noise((set_od_temp + 12.0f), 0.5f);
		sim_od_discharge = add_noise((set_od_temp + 70.0f), 1.5f);

		sim_comp_amps = 8.5f;
		if (has_faults && faults[45]) sim_comp_amps -= 1.2f;
		if (has_faults && faults[40]) sim_comp_amps += 1.4f;
		if (has_faults && faults[44]) sim_comp_amps -= 0.8f;
	} else {
        sim_comp_amps = 0.0f;
		sim_od_low_press = add_noise(125.0f, 0.5f);
		sim_od_high_press = add_noise(130.0f, 0.5f);
		sim_od_liquid_press = sim_od_high_press;
		sim_od_suction_temp = add_noise(set_id_temp, 0.3f);
		sim_od_liquid_temp = add_noise(set_od_temp, 0.3f);
		sim_od_discharge = add_noise(set_od_temp + 8.0f, 0.4f);
	}

    sim_id_supply_temp = add_noise(set_id_temp, 0.2f); // Revert to simple ambient tracking

	// Refrigerant-specific pressure switch model with hysteresis.
	float lps_trip = 40.0f;
	float lps_reset = 80.0f;
	float hps_trip = 610.0f;
	float hps_reset = 475.0f;

	if (current_refrigerant == "R454B") {
		lps_trip = 35.0f; lps_reset = 70.0f; hps_trip = 575.0f; hps_reset = 450.0f;
	} else if (current_refrigerant == "R32") {
		lps_trip = 45.0f; lps_reset = 85.0f; hps_trip = 640.0f; hps_reset = 500.0f;
	} else if (current_refrigerant == "R22") {
		lps_trip = 25.0f; lps_reset = 60.0f; hps_trip = 400.0f; hps_reset = 300.0f;
	} else if (current_refrigerant == "R407C") {
		lps_trip = 25.0f; lps_reset = 55.0f; hps_trip = 420.0f; hps_reset = 320.0f;
	} else if (current_refrigerant == "R134a") {
		lps_trip = 10.0f; lps_reset = 25.0f; hps_trip = 300.0f; hps_reset = 200.0f;
	} else if (current_refrigerant == "R404A") {
		lps_trip = 15.0f; lps_reset = 35.0f; hps_trip = 450.0f; hps_reset = 350.0f;
	}

	if (sim_od_low_press <= lps_trip) {
		phys_lps_tripped = true;
	} else if (sim_od_low_press >= lps_reset) {
		phys_lps_tripped = false;
	}

	if (sim_od_high_press >= hps_trip) {
		phys_hps_tripped = true;
	} else if (sim_od_high_press <= hps_reset) {
		phys_hps_tripped = false;
	}

	// Fault-injected switch opens still force a tripped state.
	bool lps_board_open = has_faults && (faults[7] || faults[26]);
	bool hps_board_open = has_faults && (faults[8] || faults[27]);
	if (lps_board_open) {
		phys_lps_tripped = true;
	}
	if (hps_board_open) {
		phys_hps_tripped = true;
	}

	telemetry_timer = millis();
}
