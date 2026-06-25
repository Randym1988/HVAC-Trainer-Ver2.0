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
	  sim_id_rh(50.0f) {}

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

void PhysicsEngine::update(bool y_call, bool w_call, bool g_call, bool heat_blower_on, const bool* faults) {
	bool has_faults = faults != nullptr;
	bool compressor_failed = has_faults && (faults[4] || faults[31]);
	bool cooling_call = y_call && !compressor_failed;
	bool indoor_fan_failed = has_faults && faults[24];
	bool outdoor_fan_failed = has_faults && faults[6];
	bool blower_running = (g_call || heat_blower_on) && !indoor_fan_failed;

	sim_od_ambient = set_od_temp;
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
		sim_id_supply_temp = add_noise((set_id_temp - 16.0f), 0.5f);

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
		sim_id_supply_temp = add_noise(set_id_temp - (w_call ? -18.0f : 1.0f), 0.5f);
	}

	bool lps_board_open = has_faults && (faults[7] || faults[26]);
	bool hps_board_open = has_faults && (faults[8] || faults[27]);
	phys_lps_tripped = lps_board_open || sim_od_low_press < 45.0f;
	phys_hps_tripped = hps_board_open || sim_od_high_press > 430.0f;

	telemetry_timer = millis();
}
