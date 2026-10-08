#include "PhysicsEngine.h"
#include "RefrigerantPressure.h"
#include <math.h>

const float PhysicsEngine::kElectricalTimeConstantSeconds = 0.5f;
const float PhysicsEngine::kPressureTimeConstantSeconds = 5.0f;
const float PhysicsEngine::kThermalMassTimeConstantSeconds = 30.0f;
const float PhysicsEngine::kEqualizationTimeConstantSeconds = 120.0f;

namespace {
constexpr float kOutdoorDesignCfm = 3000.0f;
constexpr float kBtuPerTon = 12000.0f;
constexpr float kIndoorCfmPerTon = 420.0f;
constexpr float kCompressorRatedCoolingCapacityBtuPerHour = 36000.0f;
// Rated indoor airflow for the 3-ton nameplate.
constexpr float kRatedIndoorCfm =
	(kCompressorRatedCoolingCapacityBtuPerHour / kBtuPerTon) * kIndoorCfmPerTon;
constexpr float kCompressorMapRatedCapacityBtuPerHour = 29400.0f;
constexpr float kEvaporatorReferenceCapacityBtuPerHour =
	kCompressorMapRatedCapacityBtuPerHour;
constexpr float kCondenserAirEffectiveness = 0.85f;
constexpr float kNominalCompressorCapacityBtuPerHour =
	kCompressorMapRatedCapacityBtuPerHour;
constexpr float kLatentLoadFractionAt50Rh = 0.25f;
constexpr float kIndoorCoilAirEffectiveness = 0.60f;
constexpr char kCompressorModelName[] = "Copeland ZP29K6E-PFV (performance chart 511570-230)";

struct RefrigerantModel {
	const char* name;
	float capacity_multiplier;
	float mass_flow_multiplier;
};

constexpr RefrigerantModel kRefrigerants[] = {
	{"R410A", 1.00f, 1.00f},
	{"R32", 1.04f, 0.78f},
	{"R454B", 0.98f, 0.97f},
	{"R22", 0.93f, 1.22f},
	{"R404A", 0.88f, 1.18f},
	{"R134a", 0.85f, 1.45f},
	{"R407C", 0.90f, 1.02f},
};

struct CompressorMapPoint {
	float capacity_btu_per_hour;
	float power_watts;
	float current_amps;
	float mass_flow_lb_per_hour;
};

struct CompressorMapRow {
	float condensing_temp_f;
	uint8_t first_evaporating_index;
	uint8_t point_count;
	CompressorMapPoint points[9];
};

constexpr float kEvaporatingTemperaturesF[9] = {
	-10.0f, 0.0f, 10.0f, 20.0f, 30.0f, 40.0f, 45.0f, 50.0f, 55.0f,
};

// Copeland chart 511570-230, HFC-410A, 60 Hz, 20 F superheat, 15 F subcooling.
// Each row stores contiguous chart points starting at first_evaporating_index.
constexpr CompressorMapRow kCompressorMap[] = {
	{80.0f, 0, 9, {
		{12600.0f, 1560.0f, 6.9f, 151.0f}, {15850.0f, 1560.0f, 6.9f, 187.0f},
		{19800.0f, 1555.0f, 6.9f, 231.0f}, {24500.0f, 1530.0f, 6.8f, 282.0f},
		{29900.0f, 1490.0f, 6.6f, 341.0f}, {36200.0f, 1420.0f, 6.3f, 408.0f},
		{39600.0f, 1375.0f, 6.2f, 444.0f}, {43300.0f, 1320.0f, 6.0f, 483.0f},
		{47100.0f, 1255.0f, 5.9f, 524.0f},
	}},
	{90.0f, 0, 9, {
		{11500.0f, 1750.0f, 7.8f, 144.0f}, {14700.0f, 1755.0f, 7.8f, 182.0f},
		{18550.0f, 1750.0f, 7.7f, 227.0f}, {23200.0f, 1740.0f, 7.7f, 279.0f},
		{28500.0f, 1715.0f, 7.5f, 340.0f}, {34700.0f, 1665.0f, 7.4f, 409.0f},
		{38000.0f, 1635.0f, 7.2f, 447.0f}, {41600.0f, 1595.0f, 7.1f, 486.0f},
		{45400.0f, 1545.0f, 7.0f, 528.0f},
	}},
	{100.0f, 0, 9, {
		{10400.0f, 1980.0f, 8.8f, 138.0f}, {13500.0f, 1975.0f, 8.7f, 176.0f},
		{17250.0f, 1970.0f, 8.7f, 222.0f}, {21700.0f, 1960.0f, 8.6f, 276.0f},
		{26900.0f, 1945.0f, 8.5f, 338.0f}, {32900.0f, 1910.0f, 8.4f, 408.0f},
		{36200.0f, 1885.0f, 8.3f, 446.0f}, {39700.0f, 1855.0f, 8.2f, 487.0f},
		{43400.0f, 1820.0f, 8.1f, 530.0f},
	}},
	{110.0f, 1, 7, {
		{15950.0f, 2230.0f, 9.8f, 217.0f}, {20200.0f, 2220.0f, 9.8f, 271.0f},
		{25200.0f, 2200.0f, 9.6f, 333.0f}, {31000.0f, 2170.0f, 9.5f, 405.0f},
		{34100.0f, 2150.0f, 9.4f, 444.0f}, {37500.0f, 2130.0f, 9.4f, 485.0f},
		{41100.0f, 2100.0f, 9.3f, 529.0f},
	}},
	{115.0f, 1, 7, {
		{15300.0f, 2380.0f, 10.5f, 214.0f}, {19450.0f, 2360.0f, 10.4f, 268.0f},
		{24300.0f, 2340.0f, 10.3f, 331.0f}, {29900.0f, 2310.0f, 10.1f, 402.0f},
		{33000.0f, 2300.0f, 10.1f, 442.0f}, {36300.0f, 2270.0f, 10.0f, 483.0f},
		{39800.0f, 2250.0f, 9.9f, 527.0f},
	}},
	{130.0f, 4, 5, {
		{21400.0f, 2850.0f, 12.5f, 321.0f}, {26600.0f, 2810.0f, 12.3f, 393.0f},
		{29400.0f, 2790.0f, 12.3f, 433.0f}, {32500.0f, 2770.0f, 12.2f, 475.0f},
		{35700.0f, 2750.0f, 12.1f, 520.0f},
	}},
	{140.0f, 5, 4, {
		{24200.0f, 3230.0f, 14.2f, 386.0f}, {26800.0f, 3200.0f, 14.1f, 425.0f},
		{29700.0f, 3170.0f, 14.0f, 467.0f}, {32700.0f, 3140.0f, 13.8f, 512.0f},
	}},
	{145.0f, 5, 4, {
		{22900.0f, 3460.0f, 15.3f, 382.0f}, {25500.0f, 3430.0f, 15.1f, 421.0f},
		{28200.0f, 3400.0f, 15.0f, 463.0f}, {31200.0f, 3370.0f, 14.9f, 508.0f},
	}},
};

CompressorMapPoint interpolateMapPoint(const CompressorMapPoint& lower,
									   const CompressorMapPoint& upper,
									   float fraction) {
	return {
		lower.capacity_btu_per_hour +
			(upper.capacity_btu_per_hour - lower.capacity_btu_per_hour) * fraction,
		lower.power_watts + (upper.power_watts - lower.power_watts) * fraction,
		lower.current_amps + (upper.current_amps - lower.current_amps) * fraction,
		lower.mass_flow_lb_per_hour +
			(upper.mass_flow_lb_per_hour - lower.mass_flow_lb_per_hour) * fraction,
	};
}

CompressorMapPoint evaluateMapRow(const CompressorMapRow& row,
								  float evaporating_temp_f) {
	const uint8_t last_index = row.first_evaporating_index + row.point_count - 1;
	if (evaporating_temp_f <= kEvaporatingTemperaturesF[row.first_evaporating_index]) {
		return row.points[0];
	}
	if (evaporating_temp_f >= kEvaporatingTemperaturesF[last_index]) {
		return row.points[row.point_count - 1];
	}
	for (uint8_t offset = 0; offset < row.point_count - 1; ++offset) {
		const uint8_t lower_index = row.first_evaporating_index + offset;
		const float lower_temp = kEvaporatingTemperaturesF[lower_index];
		const float upper_temp = kEvaporatingTemperaturesF[lower_index + 1];
		if (evaporating_temp_f <= upper_temp) {
			const float fraction = (evaporating_temp_f - lower_temp) /
				(upper_temp - lower_temp);
			return interpolateMapPoint(row.points[offset], row.points[offset + 1],
				fraction);
		}
	}
	return row.points[row.point_count - 1];
}

CompressorMapPoint evaluateCompressorMap(float evaporating_temp_f,
										 float condensing_temp_f) {
	const uint8_t row_count = sizeof(kCompressorMap) / sizeof(kCompressorMap[0]);
	if (condensing_temp_f <= kCompressorMap[0].condensing_temp_f) {
		return evaluateMapRow(kCompressorMap[0], evaporating_temp_f);
	}
	if (condensing_temp_f >= kCompressorMap[row_count - 1].condensing_temp_f) {
		return evaluateMapRow(kCompressorMap[row_count - 1], evaporating_temp_f);
	}
	for (uint8_t index = 0; index < row_count - 1; ++index) {
		const CompressorMapRow& lower_row = kCompressorMap[index];
		const CompressorMapRow& upper_row = kCompressorMap[index + 1];
		if (condensing_temp_f <= upper_row.condensing_temp_f) {
			const float fraction = (condensing_temp_f - lower_row.condensing_temp_f) /
				(upper_row.condensing_temp_f - lower_row.condensing_temp_f);
			return interpolateMapPoint(
				evaluateMapRow(lower_row, evaporating_temp_f),
				evaluateMapRow(upper_row, evaporating_temp_f), fraction);
		}
	}
	return evaluateMapRow(kCompressorMap[row_count - 1], evaporating_temp_f);
}

const RefrigerantModel& refrigerantModel(const String& name) {
	for (const RefrigerantModel& model : kRefrigerants) {
		if (name.equalsIgnoreCase(model.name)) return model;
	}
	return kRefrigerants[0];
}

float smoothToward(float current, float target, float dt, float time_constant) {
	if (dt <= 0.0f || time_constant <= 0.0f) return current;
	const float alpha = 1.0f - expf(-dt / time_constant);
	return current + (target - current) * alpha;
}

void getPressureSwitchLimits(const String& refrigerant, float& lps_trip,
							 float& lps_reset, float& hps_trip, float& hps_reset) {
	lps_trip = 40.0f;
	lps_reset = 80.0f;
	hps_trip = 610.0f;
	hps_reset = 475.0f;
	if (refrigerant == "R454B") {
		lps_trip = 35.0f; lps_reset = 70.0f; hps_trip = 575.0f; hps_reset = 450.0f;
	} else if (refrigerant == "R32") {
		lps_trip = 45.0f; lps_reset = 85.0f; hps_trip = 640.0f; hps_reset = 500.0f;
	} else if (refrigerant == "R22") {
		lps_trip = 25.0f; lps_reset = 60.0f; hps_trip = 400.0f; hps_reset = 300.0f;
	} else if (refrigerant == "R407C") {
		lps_trip = 25.0f; lps_reset = 55.0f; hps_trip = 420.0f; hps_reset = 320.0f;
	} else if (refrigerant == "R134a") {
		lps_trip = 10.0f; lps_reset = 25.0f; hps_trip = 300.0f; hps_reset = 200.0f;
	} else if (refrigerant == "R404A") {
		lps_trip = 15.0f; lps_reset = 35.0f; hps_trip = 450.0f; hps_reset = 350.0f;
	}
}
}

float PhysicsEngine::evaluateCapacityMap(float suction_temp_f, float discharge_temp_f) {
	return evaluateCompressorMap(suction_temp_f, discharge_temp_f)
		.capacity_btu_per_hour;
}

float PhysicsEngine::evaluatePowerMap(float suction_temp_f, float discharge_temp_f) {
	return evaluateCompressorMap(suction_temp_f, discharge_temp_f).power_watts;
}

float PhysicsEngine::evaluateCurrentMap(float suction_temp_f, float discharge_temp_f) {
	return evaluateCompressorMap(suction_temp_f, discharge_temp_f).current_amps;
}

float PhysicsEngine::evaluateMassFlowMap(float suction_temp_f, float discharge_temp_f) {
	return evaluateCompressorMap(suction_temp_f, discharge_temp_f)
		.mass_flow_lb_per_hour;
}

const char* PhysicsEngine::getCompressorModelName() const {
	return kCompressorModelName;
}

float PhysicsEngine::tempToPressure(float temp_f, String refrigerant) const {
	return tempToPressure(temp_f, refrigerant, false);
}

float PhysicsEngine::tempToPressure(float temp_f, String refrigerant, bool dew_point) const {
	float pressure_psig = 0.0f;
	if (!refrigerant_pressure::saturationPressurePsig(
			refrigerant, temp_f, dew_point, pressure_psig)) {
		return 0.0f;
	}
	return constrain(pressure_psig, 0.0f, 650.0f);
}

float PhysicsEngine::pressureToTemp(float psi, String refrigerant) const {
	float bubble_temp_f = 0.0f;
	float dew_temp_f = 0.0f;
	if (!refrigerant_pressure::saturationTemperatureF(
			refrigerant, psi, false, bubble_temp_f) ||
		!refrigerant_pressure::saturationTemperatureF(
			refrigerant, psi, true, dew_temp_f)) {
		return -40.0f;
	}
	return constrain((bubble_temp_f + dew_temp_f) * 0.5f, -40.0f, 150.0f);
}

PhysicsEngine::PhysicsEngine()
	: telemetry_timer(0),
	  comp_start_time(0),
	  last_update_time(0),
	  last_comp_state(false),
	  sim_comp_amps(0.0f),
	  sim_od_fan_amps(0.0f),
	  sim_id_fan_amps(0.0f),
	  sim_od_low_press(0.0f),
	  sim_od_high_press(0.0f),
	  sim_od_liquid_press(0.0f),
	  sim_od_suction_temp(0.0f),
	  sim_od_liquid_temp(0.0f),
	  sim_od_discharge(0.0f),
	  sim_od_ambient(95.0f),
	  sim_id_ambient(75.0f),
	  sim_id_return_temp(75.0f),
	  sim_id_supply_temp(75.0f),
	  sim_id_rh(50.0f),
	  current_comp_amps(0.0f),
	  current_od_fan_amps(0.0f),
	  current_id_fan_amps(0.0f),
	  current_low_pressure(0.0f),
	  current_high_pressure(0.0f),
	  current_liquid_pressure(0.0f),
	  current_evap_temp_f(75.0f),
	  current_cond_temp_f(95.0f),
	  current_id_return_temp_f(75.0f),
	  current_id_supply_temp_f(75.0f),
	  current_id_rh(50.0f),
	  compressor_capacity_btu_per_hour(0.0f),
	  compressor_mass_flow_lb_per_hour(0.0f),
	  compressor_power_watts(0.0f),
	  phys_lps_tripped(false),
	  phys_hps_tripped(false),
	  current_refrigerant("R410A"),
	  id_is_txv(true),
	  set_od_temp(95.0f),
	  set_id_temp(75.0f),
	  set_rh(50.0f),
	  flame_active(false),
	  blower_running(false),
	  high_limit_tripped(false),
	  gas_valve_on_time(0),
	  ignition_timer(0),
	  simulated_cfm(0.0f),
	  static_pressure(0.0f),
	  telemetry_state("Idle") {
	reset();
}

void PhysicsEngine::begin() {
	telemetry_timer = millis();
	reset();
}

void PhysicsEngine::reset() {
	const float equalized_temp = (set_od_temp + set_id_temp) * 0.5f;
	current_evap_temp_f = equalized_temp;
	current_cond_temp_f = equalized_temp;
	const float equalized_pressure = 0.5f * (
		tempToPressure(equalized_temp, current_refrigerant, true) +
		tempToPressure(equalized_temp, current_refrigerant, false));
	current_low_pressure = constrain(equalized_pressure, 0.0f, 250.0f);
	current_high_pressure = constrain(equalized_pressure, 0.0f, 650.0f);
	current_liquid_pressure = current_high_pressure;
	sat_suction_temp = equalized_temp;
	sat_discharge_temp = equalized_temp;
	current_id_return_temp_f = set_id_temp;
	current_id_supply_temp_f = set_id_temp;
	current_id_rh = set_rh;
	current_comp_amps = 0.0f;
	current_od_fan_amps = 0.0f;
	current_id_fan_amps = 0.0f;
	compressor_capacity_btu_per_hour = 0.0f;
	compressor_mass_flow_lb_per_hour = 0.0f;
	compressor_power_watts = 0.0f;
	phys_lps_tripped = false;
	phys_hps_tripped = false;
	last_comp_state = false;
	comp_start_time = 0;
	last_update_time = millis();
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
	sim_id_supply_temp = set_id_temp;
	sim_id_rh = set_rh;
	sim_od_low_press = current_low_pressure;
	sim_od_high_press = current_high_pressure;
	sim_od_liquid_press = current_liquid_pressure;
	sim_od_suction_temp = current_evap_temp_f;
	sim_od_liquid_temp = current_cond_temp_f;
	sim_od_discharge = current_cond_temp_f + 8.0f;
}

void PhysicsEngine::setAmbient(float od_temp, float id_temp, float rh) {
	set_od_temp = constrain(od_temp, -40.0f, 140.0f);
	set_id_temp = constrain(id_temp, -40.0f, 140.0f);
	set_rh = constrain(rh, 0.0f, 100.0f);
}

void PhysicsEngine::setRefrigerant(String type, bool is_txv) {
	current_refrigerant = "R410A";
	for (const RefrigerantModel& model : kRefrigerants) {
		if (type.equalsIgnoreCase(model.name)) {
			current_refrigerant = model.name;
			break;
		}
	}
	id_is_txv = is_txv;
}

float PhysicsEngine::add_noise(float base, float variance) {
	const float random_unit = static_cast<float>(esp_random() % 1000) / 999.0f;
	return base + ((random_unit * 2.0f - 1.0f) * variance);
}

void PhysicsEngine::update(bool y_call, bool w_call, bool g_call,
						   bool physical_blower_on, const bool* faults) {
	const bool has_faults = faults != nullptr;
	const uint32_t now = millis();
	const uint32_t elapsed_ms = now - last_update_time;
	last_update_time = now;
	const float dt = constrain(static_cast<float>(elapsed_ms) / 1000.0f, 0.0f, 0.25f);
	const RefrigerantModel& refrigerant = refrigerantModel(current_refrigerant);

	const bool compressor_failed = has_faults && (faults[4] || faults[31]);
	const bool pressure_switch_open = phys_lps_tripped || phys_hps_tripped ||
		(has_faults && (faults[7] || faults[8] || faults[26] || faults[27]));
	const bool compressor_running = y_call && !compressor_failed && !pressure_switch_open;
	const bool indoor_fan_failed = has_faults && faults[24];
	const bool outdoor_fan_failed = has_faults && faults[6];
	const bool gas_valve_active = w_call;

	blower_running = physical_blower_on && !indoor_fan_failed;

	if (gas_valve_active && gas_valve_on_time == 0) {
		gas_valve_on_time = now == 0 ? 1 : now;
		ignition_timer = now + 2000;
		telemetry_state = "Ignition Delay";
	} else if (!gas_valve_active) {
		gas_valve_on_time = 0;
		ignition_timer = 0;
		flame_active = false;
		telemetry_state = "Idle";
	}
	if (ignition_timer != 0 && static_cast<int32_t>(now - ignition_timer) >= 0) {
		flame_active = true;
		telemetry_state = "Burners Lit";
		ignition_timer = 0;
	}
	if (has_faults && faults[22]) high_limit_tripped = true;
	if (gas_valve_active && gas_valve_on_time != 0 &&
		now - gas_valve_on_time > 90000 && !blower_running) {
		high_limit_tripped = true;
		telemetry_state = "High Limit Tripped";
	}

	float target_cfm = 0.0f;
	if (blower_running) {
		target_cfm = kRatedIndoorCfm;
		if (has_faults && faults[46]) target_cfm = 750.0f;
		else if (has_faults && faults[47]) target_cfm = 1800.0f;
	}
	float target_static_pressure = 0.0f;
	if (blower_running) {
		target_static_pressure = 0.5f;
		if (has_faults && faults[46]) target_static_pressure = 0.8f;
		else if (has_faults && faults[47]) target_static_pressure = 0.35f;
	}
	simulated_cfm = smoothToward(simulated_cfm, target_cfm, dt,
		kElectricalTimeConstantSeconds);
	static_pressure = smoothToward(static_pressure, target_static_pressure, dt,
		kElectricalTimeConstantSeconds);

	if (compressor_running && !last_comp_state) comp_start_time = now;
	last_comp_state = compressor_running;

	float evaporator_flow_factor = 1.0f;
	float compressor_capacity_factor = 1.0f;
	float compressor_efficiency_factor = 1.0f;
	float condenser_heat_transfer_factor = 1.0f;
	float superheat_f = id_is_txv ? 15.0f : 20.0f;
	const float subcool_f = 8.0f;
	// 1.0 = factory charge, 0.7 = 30% lost, 1.3 = 30% extra (faults 55/56).
	float system_charge_ratio = 1.0f;
	if (has_faults && faults[55]) system_charge_ratio = 0.7f;
	if (has_faults && faults[56]) system_charge_ratio = 1.3f;
	if (system_charge_ratio < 0.9f) {
		// Starved evaporator: less refrigerant reaches the metering device.
		superheat_f += (1.0f - system_charge_ratio) * 40.0f;
	}
	if (has_faults && faults[40]) condenser_heat_transfer_factor = 0.72f;
	if (has_faults && faults[41]) {
		evaporator_flow_factor = 1.08f;
		superheat_f = 6.0f;
	}
	if (has_faults && faults[42]) {
		evaporator_flow_factor = 0.68f;
		superheat_f = 22.0f;
	}
	if (has_faults && faults[43]) {
		evaporator_flow_factor = 0.82f;
		superheat_f = 18.0f;
	}
	if (has_faults && faults[44]) compressor_capacity_factor = 0.65f;
	if (has_faults && faults[45]) {
		compressor_capacity_factor = 0.85f;
		compressor_efficiency_factor = 0.75f;
	}

	float sensible_load_btu_per_hour = 0.0f;
	float latent_load_btu_per_hour = 0.0f;
	float heat_absorbed_btu_per_hour = 0.0f;
	float heat_rejected_btu_per_hour = 0.0f;
	float condenser_heat_transfer_btu_per_hour = 0.0f;
	float target_id_supply_temp = set_id_temp;
	float outdoor_cfm = 0.0f;
	float compressor_map_current_amps = 0.0f;

	if (compressor_running) {
		const float air_to_coil_delta = max(set_id_temp - current_evap_temp_f, 0.0f);
		const float sensible_delta_t = air_to_coil_delta * kIndoorCoilAirEffectiveness;
		sensible_load_btu_per_hour = 1.08f * simulated_cfm * sensible_delta_t;
		const float humidity_load = max((set_rh - 30.0f) / 20.0f, 0.0f);
		latent_load_btu_per_hour = sensible_load_btu_per_hour *
			kLatentLoadFractionAt50Rh * humidity_load;
		heat_absorbed_btu_per_hour =
			(sensible_load_btu_per_hour + latent_load_btu_per_hour) *
			evaporator_flow_factor;
		if (simulated_cfm > 1.0f) {
			target_id_supply_temp = set_id_temp - sensible_delta_t;
		}

		const CompressorMapPoint compressor_map = evaluateCompressorMap(
			current_evap_temp_f, current_cond_temp_f);
		compressor_map_current_amps = compressor_map.current_amps;
		compressor_capacity_btu_per_hour = constrain(
			compressor_map.capacity_btu_per_hour * refrigerant.capacity_multiplier *
				compressor_capacity_factor,
			0.0f, 60000.0f);
		compressor_mass_flow_lb_per_hour = constrain(
			compressor_map.mass_flow_lb_per_hour * refrigerant.mass_flow_multiplier *
				evaporator_flow_factor,
			0.0f, 1200.0f);
		compressor_power_watts = constrain(
			compressor_map.power_watts * refrigerant.capacity_multiplier *
				compressor_capacity_factor / compressor_efficiency_factor,
			0.0f, 12000.0f);
		heat_rejected_btu_per_hour =
			compressor_capacity_btu_per_hour + compressor_power_watts * 3.412f;

		if (!outdoor_fan_failed) outdoor_cfm = kOutdoorDesignCfm;
		const float condenser_approach_f = max(current_cond_temp_f - set_od_temp, 0.0f);
		// Liquid stacking in an overcharged condenser reduces active surface.
		float charge_ua_factor = 1.0f;
		if (system_charge_ratio > 1.0f) {
			charge_ua_factor = 1.0f - (system_charge_ratio - 1.0f) * 0.8f;
		}
		condenser_heat_transfer_btu_per_hour =
			1.08f * outdoor_cfm * condenser_approach_f *
			kCondenserAirEffectiveness * condenser_heat_transfer_factor *
			charge_ua_factor;

		const float evaporator_imbalance =
			heat_absorbed_btu_per_hour - compressor_capacity_btu_per_hour;
		const float condenser_imbalance =
			heat_rejected_btu_per_hour - condenser_heat_transfer_btu_per_hour;
		current_evap_temp_f += (evaporator_imbalance /
			kEvaporatorReferenceCapacityBtuPerHour) *
			(dt / kThermalMassTimeConstantSeconds) * 35.0f;
		current_cond_temp_f += (condenser_imbalance /
			kNominalCompressorCapacityBtuPerHour) *
			(dt / kThermalMassTimeConstantSeconds) * 25.0f;
		current_evap_temp_f = constrain(current_evap_temp_f, -40.0f, 150.0f);
		current_cond_temp_f = constrain(current_cond_temp_f, -40.0f, 180.0f);
	} else {
		compressor_capacity_btu_per_hour = 0.0f;
		compressor_mass_flow_lb_per_hour = 0.0f;
		compressor_power_watts = 0.0f;
		const float equalized_ambient_temp = (set_id_temp + set_od_temp) * 0.5f;
		current_evap_temp_f += (equalized_ambient_temp - current_evap_temp_f) *
			(dt / kEqualizationTimeConstantSeconds);
		current_cond_temp_f += (equalized_ambient_temp - current_cond_temp_f) *
			(dt / kEqualizationTimeConstantSeconds);
		current_evap_temp_f = constrain(current_evap_temp_f, -40.0f, 150.0f);
		current_cond_temp_f = constrain(current_cond_temp_f, -40.0f, 180.0f);
		if (flame_active && blower_running) target_id_supply_temp = set_id_temp + 45.0f;
	}

	sat_suction_temp = current_evap_temp_f;
	sat_discharge_temp = current_cond_temp_f;
	const float current_superheat = compressor_running ? superheat_f : 0.0f;
	float current_subcool = 0.0f;
	if (compressor_running) {
		current_subcool = subcool_f;
		if (system_charge_ratio > 1.0f) {
			current_subcool += (system_charge_ratio - 1.0f) * 45.0f;
		} else if (system_charge_ratio < 0.9f) {
			current_subcool -= (1.0f - system_charge_ratio) * 50.0f;
		}
		// Reduced outdoor airflow leaves no heat sink to subcool the liquid.
		current_subcool *= outdoor_cfm / kOutdoorDesignCfm;
		current_subcool = constrain(current_subcool, 0.0f, 40.0f);
	}
	float target_suction_temp = current_evap_temp_f + current_superheat;
	// Liquid cannot be colder than outdoor air plus a 3 F approach.
	const float target_liquid_temp = compressor_running
		? max(current_cond_temp_f - current_subcool, set_od_temp + 3.0f)
		: current_cond_temp_f;
	const float target_discharge_temp = compressor_running
		? current_cond_temp_f + 35.0f + 0.15f *
			max(current_cond_temp_f - current_evap_temp_f, 0.0f)
		: current_cond_temp_f + 8.0f;

	float target_comp_amps = compressor_map_current_amps *
		refrigerant.capacity_multiplier * compressor_capacity_factor /
		compressor_efficiency_factor;
	if (compressor_running && now - comp_start_time < 1000) target_comp_amps += 3.0f;
	const float target_od_fan_amps = compressor_running && !outdoor_fan_failed ? 0.9f : 0.0f;
	const float target_id_fan_amps = blower_running ? 3.8f : 0.0f;

	current_comp_amps = smoothToward(current_comp_amps, target_comp_amps, dt,
		kElectricalTimeConstantSeconds);
	current_od_fan_amps = smoothToward(current_od_fan_amps, target_od_fan_amps, dt,
		kElectricalTimeConstantSeconds);
	current_id_fan_amps = smoothToward(current_id_fan_amps, target_id_fan_amps, dt,
		kElectricalTimeConstantSeconds);
	current_id_return_temp_f = smoothToward(current_id_return_temp_f, set_id_temp,
		dt, kThermalMassTimeConstantSeconds);
	current_id_supply_temp_f = smoothToward(current_id_supply_temp_f,
		target_id_supply_temp, dt, kThermalMassTimeConstantSeconds);
	current_id_rh = smoothToward(current_id_rh, set_rh, dt,
		kThermalMassTimeConstantSeconds);

	float lps_trip, lps_reset, hps_trip, hps_reset;
	getPressureSwitchLimits(current_refrigerant, lps_trip, lps_reset,
		hps_trip, hps_reset);
	float target_low_pressure = tempToPressure(
		current_evap_temp_f, current_refrigerant, true);
	float target_high_pressure = tempToPressure(
		current_cond_temp_f, current_refrigerant, false);
	if (compressor_running && !blower_running) {
		target_low_pressure = min(target_low_pressure, lps_trip * 0.75f);
		sat_suction_temp = pressureToTemp(target_low_pressure, current_refrigerant);
		target_suction_temp = sat_suction_temp + current_superheat;
	}
	if (!compressor_running) {
		const float equalized_pressure =
			(target_low_pressure + target_high_pressure) * 0.5f;
		target_low_pressure = equalized_pressure;
		target_high_pressure = equalized_pressure;
		sat_suction_temp = pressureToTemp(equalized_pressure, current_refrigerant);
		sat_discharge_temp = sat_suction_temp;
	}
	const float pressure_drop_psi = compressor_running
		? constrain(0.5f + 0.004f * compressor_mass_flow_lb_per_hour, 0.5f, 8.0f)
		: 0.0f;
	const float target_liquid_pressure = target_high_pressure - pressure_drop_psi;
	const float pressure_time_constant = compressor_running
		? kPressureTimeConstantSeconds
		: kEqualizationTimeConstantSeconds;
	current_low_pressure = constrain(smoothToward(current_low_pressure,
		target_low_pressure, dt, pressure_time_constant), 0.0f, 250.0f);
	current_high_pressure = constrain(smoothToward(current_high_pressure,
		target_high_pressure, dt, pressure_time_constant), 0.0f, 650.0f);
	current_liquid_pressure = constrain(smoothToward(current_liquid_pressure,
		target_liquid_pressure, dt, pressure_time_constant), 0.0f, 650.0f);
	const float low_pressure = current_low_pressure;
	const float high_pressure = current_high_pressure;
	const float liquid_pressure = current_liquid_pressure;

	// Noise is added before the final clamps so published gauges stay in range.
	sim_comp_amps = constrain(add_noise(current_comp_amps, 0.08f), 0.0f, 40.0f);
	sim_od_fan_amps = constrain(add_noise(current_od_fan_amps, 0.03f), 0.0f, 10.0f);
	sim_id_fan_amps = constrain(add_noise(current_id_fan_amps, 0.08f), 0.0f, 15.0f);
	sim_od_low_press = constrain(add_noise(low_pressure, 0.4f), 0.0f, 250.0f);
	sim_od_high_press = constrain(add_noise(high_pressure, 0.6f), 0.0f, 650.0f);
	sim_od_liquid_press = constrain(add_noise(liquid_pressure, 0.5f), 0.0f, 650.0f);
	sim_od_suction_temp = constrain(add_noise(target_suction_temp, 0.4f), -40.0f, 250.0f);
	sim_od_liquid_temp = constrain(add_noise(target_liquid_temp, 0.5f), -40.0f, 250.0f);
	sim_od_discharge = constrain(add_noise(target_discharge_temp, 1.0f), -40.0f, 300.0f);
	sim_od_ambient = constrain(set_od_temp, -40.0f, 140.0f);
	sim_id_ambient = constrain(set_id_temp, -40.0f, 140.0f);
	sim_id_return_temp = constrain(add_noise(current_id_return_temp_f, 0.2f), -40.0f, 160.0f);
	sim_id_supply_temp = constrain(add_noise(current_id_supply_temp_f, 0.3f), -40.0f, 220.0f);
	sim_id_rh = constrain(add_noise(current_id_rh, 0.5f), 0.0f, 100.0f);
	simulated_cfm = constrain(simulated_cfm, 0.0f, 2000.0f);
	static_pressure = constrain(static_pressure, 0.0f, 2.0f);

	if (low_pressure <= lps_trip) phys_lps_tripped = true;
	else if (low_pressure >= lps_reset) phys_lps_tripped = false;
	if (high_pressure >= hps_trip) phys_hps_tripped = true;
	else if (high_pressure <= hps_reset) phys_hps_tripped = false;
	if (has_faults && (faults[7] || faults[26])) phys_lps_tripped = true;
	if (has_faults && (faults[8] || faults[27])) phys_hps_tripped = true;

	telemetry_timer = now;
}
