#include <Arduino.h>
#include <unity.h>

#include "PhysicsEngine.h"
#include "RefrigerantPressure.h"

namespace {
const char* kRefrigerants[] = {
	"R410A", "R22", "R32", "R454B", "R134a", "R404A", "R407C",
};

void testZp29CompressorMapAtPublishedPoint() {
	TEST_ASSERT_FLOAT_WITHIN(0.1f, 29400.0f,
		PhysicsEngine::evaluateCapacityMap(45.0f, 130.0f));
	TEST_ASSERT_FLOAT_WITHIN(0.1f, 2790.0f,
		PhysicsEngine::evaluatePowerMap(45.0f, 130.0f));
	TEST_ASSERT_FLOAT_WITHIN(0.1f, 12.3f,
		PhysicsEngine::evaluateCurrentMap(45.0f, 130.0f));
	TEST_ASSERT_FLOAT_WITHIN(0.1f, 433.0f,
		PhysicsEngine::evaluateMassFlowMap(45.0f, 130.0f));
	TEST_ASSERT_TRUE(PhysicsEngine::evaluateCapacityMap(30.0f, 130.0f) <
		PhysicsEngine::evaluateCapacityMap(45.0f, 130.0f));
	TEST_ASSERT_TRUE(PhysicsEngine::evaluateMassFlowMap(30.0f, 130.0f) <
		PhysicsEngine::evaluateMassFlowMap(45.0f, 130.0f));
	TEST_ASSERT_FLOAT_WITHIN(0.1f, 31325.0f,
		PhysicsEngine::evaluateCapacityMap(42.5f, 122.5f));
	TEST_ASSERT_FLOAT_WITHIN(0.1f, 2542.5f,
		PhysicsEngine::evaluatePowerMap(42.5f, 122.5f));
	TEST_ASSERT_FLOAT_WITHIN(0.1f, 11.175f,
		PhysicsEngine::evaluateCurrentMap(42.5f, 122.5f));
	TEST_ASSERT_FLOAT_WITHIN(0.1f, 437.75f,
		PhysicsEngine::evaluateMassFlowMap(42.5f, 122.5f));
}

void testYa31R454bCompressorMapMatchesChart() {
	CompressorOperatingPoint point = {};
	// Published point: 35 F evap / 110 F cond.
	TEST_ASSERT_TRUE(PhysicsEngine::evaluateCompressorForRefrigerant(
		"R454B", 35.0f, 110.0f, point));
	TEST_ASSERT_FLOAT_WITHIN(0.1f, 29800.0f, point.capacity_btu_per_hour);
	TEST_ASSERT_FLOAT_WITHIN(0.1f, 2240.0f, point.power_watts);
	TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.1f, point.current_amps);
	TEST_ASSERT_FLOAT_WITHIN(0.1f, 322.0f, point.mass_flow_lb_per_hour);
	// Bilinear midpoint: 45 F evap / 115 F cond.
	PhysicsEngine::evaluateCompressorForRefrigerant("R454B", 45.0f, 115.0f, point);
	TEST_ASSERT_FLOAT_WITHIN(0.1f, 35675.0f, point.capacity_btu_per_hour);
	TEST_ASSERT_FLOAT_WITHIN(0.1f, 2362.5f, point.power_watts);
	TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.625f, point.current_amps);
	TEST_ASSERT_FLOAT_WITHIN(0.1f, 390.5f, point.mass_flow_lb_per_hour);
	// R-410A keeps the ZP29K6E chart; other refrigerants scale it.
	TEST_ASSERT_TRUE(PhysicsEngine::evaluateCompressorForRefrigerant(
		"R410A", 45.0f, 130.0f, point));
	TEST_ASSERT_FLOAT_WITHIN(0.1f, 29400.0f, point.capacity_btu_per_hour);
	TEST_ASSERT_FALSE(PhysicsEngine::evaluateCompressorForRefrigerant(
		"R32", 45.0f, 130.0f, point));
	TEST_ASSERT_NOT_NULL(PhysicsEngine::heatPumpCompressorModelName("R454B"));
	TEST_ASSERT_NULL(PhysicsEngine::heatPumpCompressorModelName("R410A"));

	PhysicsEngine engine;
	engine.begin();
	engine.setRefrigerant("R454B", true);
	TEST_ASSERT_EQUAL_STRING("Copeland YA31K1E-PFV (performance chart 99949-230)",
		engine.getCompressorModelName());
}

void testCompressorElectricalFollowsRefrigerant() {
	const CompressorElectricalSpec& ya31 = PhysicsEngine::compressorElectricalSpec("R454B");
	TEST_ASSERT_EQUAL_STRING("YA31K1E-PFV", ya31.model);
	TEST_ASSERT_FLOAT_WITHIN(0.01f, 40.0f, ya31.run_cap_uf);
	TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.43f, ya31.start_winding_ohms);
	TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.72f, ya31.run_winding_ohms);
	const CompressorElectricalSpec& zp29 = PhysicsEngine::compressorElectricalSpec("R410A");
	TEST_ASSERT_EQUAL_STRING("ZP29K6E-PFV", zp29.model);
	TEST_ASSERT_FLOAT_WITHIN(0.01f, 45.0f, zp29.run_cap_uf);
	TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.58f, zp29.start_winding_ohms);
	TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.92f, zp29.run_winding_ohms);

	CompressorElectricalReading running =
		PhysicsEngine::compressorElectricalReading("R454B", 12.0f, 18.3f);
	TEST_ASSERT_FLOAT_WITHIN(0.01f, 239.67f, running.line_volts);
	TEST_ASSERT_FLOAT_WITHIN(0.05f, 320.2f, running.run_cap_volts);
	TEST_ASSERT_FLOAT_WITHIN(0.01f, 4.757f, running.start_winding_amps);
	TEST_ASSERT_FLOAT_WITHIN(0.01f, 11.874f, running.run_winding_amps);
	// Field check: start amps x 2652 / cap volts recovers the in-circuit capacitance.
	TEST_ASSERT_FLOAT_WITHIN(0.05f, 39.4f,
		running.start_winding_amps * 2652.0f / running.run_cap_volts);

	running = PhysicsEngine::compressorElectricalReading("R410A", 12.0f, 18.3f);
	TEST_ASSERT_FLOAT_WITHIN(0.05f, 44.3f, running.run_cap_uf);
	TEST_ASSERT_FLOAT_WITHIN(0.01f, 5.352f, running.start_winding_amps);

	CompressorElectricalReading stopped =
		PhysicsEngine::compressorElectricalReading("R454B", 0.0f, 4.5f);
	TEST_ASSERT_FLOAT_WITHIN(0.01f, 241.05f, stopped.line_volts);
	TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, stopped.run_cap_volts);
	TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, stopped.start_winding_amps);
	TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, stopped.run_cap_uf);
}

void testAllRefrigerantCurvesAreMonotonicAndInvertible() {
	for (const char* refrigerant : kRefrigerants) {
		float previous_bubble = -10000.0f;
		float previous_dew = -10000.0f;
		for (int temperature = -40; temperature <= 150; ++temperature) {
			float bubble_pressure = 0.0f;
			float dew_pressure = 0.0f;
			TEST_ASSERT_TRUE(refrigerant_pressure::saturationPressurePsig(
				String(refrigerant), temperature, false, bubble_pressure));
			TEST_ASSERT_TRUE(refrigerant_pressure::saturationPressurePsig(
				String(refrigerant), temperature, true, dew_pressure));
			TEST_ASSERT_TRUE(bubble_pressure > previous_bubble);
			TEST_ASSERT_TRUE(dew_pressure > previous_dew);

			float bubble_temperature = 0.0f;
			float dew_temperature = 0.0f;
			TEST_ASSERT_TRUE(refrigerant_pressure::saturationTemperatureF(
				String(refrigerant), bubble_pressure, false, bubble_temperature));
			TEST_ASSERT_TRUE(refrigerant_pressure::saturationTemperatureF(
				String(refrigerant), dew_pressure, true, dew_temperature));
			TEST_ASSERT_FLOAT_WITHIN(0.11f, temperature, bubble_temperature);
			TEST_ASSERT_FLOAT_WITHIN(0.11f, temperature, dew_temperature);
			previous_bubble = bubble_pressure;
			previous_dew = dew_pressure;
		}

		float below_range_pressure = 0.0f;
		float above_range_pressure = 0.0f;
		TEST_ASSERT_TRUE(refrigerant_pressure::saturationPressurePsig(
			String(refrigerant), -45.0f, true, below_range_pressure));
		TEST_ASSERT_TRUE(refrigerant_pressure::saturationPressurePsig(
			String(refrigerant), 155.0f, false, above_range_pressure));
	}
}

void testOffCycleEqualizationUsesSelectedRefrigerant() {
	PhysicsEngine engine;
	bool no_faults[57] = {};
	engine.begin();
	engine.setAmbient(70.0f, 70.0f, 50.0f);

	for (const char* refrigerant : kRefrigerants) {
		engine.setRefrigerant(refrigerant, true);
		engine.reset();

		float expected_pressure = 0.0f;
		TEST_ASSERT_TRUE(refrigerant_pressure::meanSaturationPressurePsig(
			String(refrigerant), 70.0f, expected_pressure));
		expected_pressure = constrain(expected_pressure, 0.0f, 250.0f);
		TEST_ASSERT_FLOAT_WITHIN(2.0f, expected_pressure, engine.getOdLowPress());
		TEST_ASSERT_FLOAT_WITHIN(2.0f, expected_pressure, engine.getOdHighPress());
		engine.update(false, false, false, false, no_faults);
		TEST_ASSERT_FLOAT_WITHIN(2.0f, expected_pressure, engine.getOdLowPress());
		TEST_ASSERT_FLOAT_WITHIN(2.0f, expected_pressure, engine.getOdHighPress());
	}
}

void testFactoryAmbientPointAndAmbientPressureTrends() {
	PhysicsEngine factory_engine;
	PhysicsEngine warmer_outdoor_engine;
	PhysicsEngine warmer_indoor_engine;
	bool no_faults[57] = {};
	PhysicsEngine* engines[] = {
		&factory_engine, &warmer_outdoor_engine, &warmer_indoor_engine,
	};
	const float outdoor_conditions[] = {95.0f, 105.0f, 95.0f};
	const float indoor_conditions[] = {75.0f, 75.0f, 85.0f};

	for (size_t index = 0; index < 3; ++index) {
		engines[index]->begin();
		engines[index]->setAmbient(
			outdoor_conditions[index], indoor_conditions[index], 50.0f);
		engines[index]->setRefrigerant("R410A", true);
	}

	for (int step = 0; step < 1200; ++step) {
		delay(100);
		for (PhysicsEngine* engine : engines) {
			engine->update(true, false, true, true, no_faults);
		}
	}

	float expected_low_pressure = 0.0f;
	float expected_high_pressure = 0.0f;
	TEST_ASSERT_TRUE(refrigerant_pressure::saturationPressurePsig(
		"R410A", factory_engine.getSatSuctionTemp(), true, expected_low_pressure));
	TEST_ASSERT_TRUE(refrigerant_pressure::saturationPressurePsig(
		"R410A", factory_engine.getSatDischargeTemp(), false, expected_high_pressure));
	TEST_ASSERT_FLOAT_WITHIN(2.0f, expected_low_pressure,
		factory_engine.getOdLowPress());
	TEST_ASSERT_FLOAT_WITHIN(2.0f, expected_high_pressure,
		factory_engine.getOdHighPress());
	TEST_ASSERT_TRUE(factory_engine.getSatSuctionTemp() >= 40.0f);
	TEST_ASSERT_TRUE(factory_engine.getSatSuctionTemp() <= 45.0f);
	TEST_ASSERT_FLOAT_WITHIN(1.0f, 15.0f, factory_engine.getSuperheat());
	TEST_ASSERT_TRUE(factory_engine.getIdReturnTemp() - factory_engine.getIdSupplyTemp() >= 17.0f);
	TEST_ASSERT_TRUE(factory_engine.getIdReturnTemp() - factory_engine.getIdSupplyTemp() <= 20.5f);
	TEST_ASSERT_FLOAT_WITHIN(1.0f, 8.0f, factory_engine.getSubcooling());
	TEST_ASSERT_TRUE(factory_engine.getCompressorCapacityBtuPerHour() > 28000.0f);
	TEST_ASSERT_TRUE(factory_engine.getCompressorCapacityBtuPerHour() < 40000.0f);
	TEST_ASSERT_TRUE(factory_engine.getCompressorMassFlowLbPerHour() > 300.0f);
	TEST_ASSERT_TRUE(factory_engine.getCompressorMassFlowLbPerHour() < 450.0f);
	TEST_ASSERT_TRUE(factory_engine.getCompressorPowerWatts() > 2200.0f);
	TEST_ASSERT_TRUE(factory_engine.getCompressorPowerWatts() < 3600.0f);
	TEST_ASSERT_EQUAL_STRING(
		"Copeland ZP29K6E-PFV (performance chart 511570-230)",
		factory_engine.getCompressorModelName());
	TEST_ASSERT_TRUE(warmer_outdoor_engine.getOdHighPress() >
		factory_engine.getOdHighPress() + 10.0f);
	TEST_ASSERT_TRUE(warmer_indoor_engine.getOdLowPress() >
		factory_engine.getOdLowPress() + 10.0f);
}

void testCoolingPressureRampsAndRespondsToAirflowFaults() {
	PhysicsEngine normal_engine;
	PhysicsEngine indoor_fan_fault_engine;
	PhysicsEngine outdoor_fan_fault_engine;
	PhysicsEngine blower_off_engine;
	PhysicsEngine low_airflow_engine;
	PhysicsEngine high_airflow_engine;
	bool normal_faults[57] = {};
	bool indoor_fan_faults[57] = {};
	bool outdoor_fan_faults[57] = {};
	bool blower_off_faults[57] = {};
	bool low_airflow_faults[57] = {};
	bool high_airflow_faults[57] = {};
	indoor_fan_faults[24] = true;
	outdoor_fan_faults[6] = true;
	low_airflow_faults[46] = true;
	high_airflow_faults[47] = true;

	PhysicsEngine* engines[] = {
		&normal_engine, &indoor_fan_fault_engine, &outdoor_fan_fault_engine,
		&blower_off_engine, &low_airflow_engine, &high_airflow_engine,
	};
	for (PhysicsEngine* engine : engines) {
		engine->begin();
		engine->setAmbient(95.0f, 75.0f, 50.0f);
		engine->setRefrigerant("R410A", true);
	}

	const float initial_low_pressure = normal_engine.getOdLowPress();
	for (int step = 0; step < 200; ++step) {
		delay(100);
		normal_engine.update(true, false, true, true, normal_faults);
		indoor_fan_fault_engine.update(true, false, true, true, indoor_fan_faults);
		outdoor_fan_fault_engine.update(true, false, true, true, outdoor_fan_faults);
		blower_off_engine.update(true, false, true, false, blower_off_faults);
		low_airflow_engine.update(true, false, true, true, low_airflow_faults);
		high_airflow_engine.update(true, false, true, true, high_airflow_faults);
		if (step == 0) {
			TEST_ASSERT_TRUE(outdoor_fan_fault_engine.getOdHighPress() <
				normal_engine.getOdHighPress() + 20.0f);
		}
	}

	TEST_ASSERT_FLOAT_WITHIN(1.0f, 1260.0f, normal_engine.getSimulatedCfm());
	TEST_ASSERT_TRUE(normal_engine.getOdLowPress() < initial_low_pressure);
	TEST_ASSERT_TRUE(indoor_fan_fault_engine.getOdLowPress() + 15.0f <
		normal_engine.getOdLowPress());
	TEST_ASSERT_TRUE(outdoor_fan_fault_engine.getOdHighPress() >
		normal_engine.getOdHighPress() + 20.0f);
	TEST_ASSERT_TRUE(outdoor_fan_fault_engine.getOdLiquidPress() >
		normal_engine.getOdLiquidPress() + 15.0f);
	TEST_ASSERT_TRUE(blower_off_engine.getOdLowPress() + 20.0f <
		normal_engine.getOdLowPress());
	TEST_ASSERT_FLOAT_WITHIN(0.1f, 0.0f, blower_off_engine.getSimulatedCfm());
	TEST_ASSERT_TRUE(indoor_fan_fault_engine.isLpsTripped());
	TEST_ASSERT_TRUE(indoor_fan_fault_engine.getOdLowPress() < 80.0f);
	TEST_ASSERT_TRUE(blower_off_engine.getIdSupplyTemp() >
		normal_engine.getIdSupplyTemp() + 5.0f);
	TEST_ASSERT_TRUE(low_airflow_engine.getSimulatedCfm() <
		normal_engine.getSimulatedCfm());
	TEST_ASSERT_TRUE(high_airflow_engine.getSimulatedCfm() >
		normal_engine.getSimulatedCfm());
	TEST_ASSERT_TRUE(low_airflow_engine.getStaticPressure() >
		normal_engine.getStaticPressure());
	TEST_ASSERT_TRUE(high_airflow_engine.getStaticPressure() <
		normal_engine.getStaticPressure());
	TEST_ASSERT_TRUE(low_airflow_engine.getIdSupplyTemp() + 5.0f <
		normal_engine.getIdSupplyTemp());
	TEST_ASSERT_TRUE(high_airflow_engine.getIdSupplyTemp() >
		normal_engine.getIdSupplyTemp() + 0.5f);
	const float head_to_liquid_pressure_drop = normal_engine.getOdHighPress() -
		normal_engine.getOdLiquidPress();
	TEST_ASSERT_TRUE(head_to_liquid_pressure_drop > 1.0f);
	TEST_ASSERT_TRUE(head_to_liquid_pressure_drop < 5.0f);
}

void testBlowerFaultPressureSwitchResetsAfterPressureRecovers() {
	PhysicsEngine engine;
	bool blower_faults[57] = {};
	bool no_faults[57] = {};
	blower_faults[24] = true;
	engine.begin();
	engine.setAmbient(95.0f, 75.0f, 50.0f);
	engine.setRefrigerant("R410A", true);

	for (int step = 0; step < 300; ++step) {
		delay(100);
		engine.update(true, false, true, true, blower_faults);
		if (engine.isLpsTripped()) break;
	}
	TEST_ASSERT_TRUE(engine.isLpsTripped());
	TEST_ASSERT_TRUE(engine.getOdLowPress() <= 41.0f);

	bool reset_after_pressure_recovery = false;
	for (int step = 0; step < 1200; ++step) {
		delay(100);
		engine.update(false, false, false, true, no_faults);
		if (!engine.isLpsTripped()) {
			TEST_ASSERT_TRUE(engine.getOdLowPress() >= 79.0f);
			reset_after_pressure_recovery = true;
			break;
		}
		TEST_ASSERT_TRUE(engine.getOdLowPress() < 81.0f);
	}
	TEST_ASSERT_TRUE(reset_after_pressure_recovery);
}

void testChargeFaultsShiftSubcoolingAndHeadPressure() {
	PhysicsEngine normal_engine;
	PhysicsEngine low_engine;
	PhysicsEngine over_engine;
	bool normal[57] = {};
	bool low[57] = {};
	bool over[57] = {};
	low[55] = true;
	over[56] = true;
	PhysicsEngine* engines[] = {&normal_engine, &low_engine, &over_engine};
	bool* fault_sets[] = {normal, low, over};
	for (PhysicsEngine* engine : engines) {
		engine->begin();
		engine->setAmbient(95.0f, 75.0f, 50.0f);
		engine->setRefrigerant("R410A", true);
	}
	for (int step = 0; step < 1200; ++step) {
		delay(100);
		for (size_t i = 0; i < 3; ++i) {
			engines[i]->update(true, false, true, true, fault_sets[i]);
		}
	}
	TEST_ASSERT_TRUE(low_engine.getSubcooling() < 2.0f);
	TEST_ASSERT_TRUE(low_engine.getSuperheat() > normal_engine.getSuperheat() + 8.0f);
	TEST_ASSERT_TRUE(over_engine.getSubcooling() > normal_engine.getSubcooling() + 10.0f);
	TEST_ASSERT_TRUE(over_engine.getOdHighPress() > normal_engine.getOdHighPress());
	TEST_ASSERT_TRUE(low_engine.getOdLiquidTemp() >= 98.0f - 2.0f);
}

void testPublishedOutputsStayWithinGaugeBounds() {
	PhysicsEngine engine;
	bool faults[57] = {};
	engine.begin();
	engine.setAmbient(150.0f, -40.0f, 120.0f);
	engine.setRefrigerant("R410A", true);
	for (int step = 0; step < 20; ++step) {
		delay(50);
		engine.update(true, false, true, true, faults);
	}

	TEST_ASSERT_TRUE(engine.getOdLowPress() >= 0.0f);
	TEST_ASSERT_TRUE(engine.getOdLowPress() <= 250.0f);
	TEST_ASSERT_TRUE(engine.getOdHighPress() >= 0.0f);
	TEST_ASSERT_TRUE(engine.getOdHighPress() <= 650.0f);
	TEST_ASSERT_TRUE(engine.getOdLiquidPress() >= 0.0f);
	TEST_ASSERT_TRUE(engine.getOdLiquidPress() <= 650.0f);
	TEST_ASSERT_TRUE(engine.getCompAmps() >= 0.0f);
	TEST_ASSERT_TRUE(engine.getCompAmps() <= 40.0f);
	TEST_ASSERT_TRUE(engine.getOdSuctionTemp() >= -40.0f);
	TEST_ASSERT_TRUE(engine.getOdSuctionTemp() <= 250.0f);
	TEST_ASSERT_TRUE(engine.getOdLiquidTemp() >= -40.0f);
	TEST_ASSERT_TRUE(engine.getOdLiquidTemp() <= 250.0f);
	TEST_ASSERT_TRUE(engine.getOdDischargeTemp() >= -40.0f);
	TEST_ASSERT_TRUE(engine.getOdDischargeTemp() <= 300.0f);
	TEST_ASSERT_TRUE(engine.getIdRh() >= 0.0f);
	TEST_ASSERT_TRUE(engine.getIdRh() <= 100.0f);
}
}

void setup() {
	delay(1000);
	UNITY_BEGIN();
	RUN_TEST(testZp29CompressorMapAtPublishedPoint);
	RUN_TEST(testYa31R454bCompressorMapMatchesChart);
	RUN_TEST(testCompressorElectricalFollowsRefrigerant);
	RUN_TEST(testAllRefrigerantCurvesAreMonotonicAndInvertible);
	RUN_TEST(testOffCycleEqualizationUsesSelectedRefrigerant);
	RUN_TEST(testFactoryAmbientPointAndAmbientPressureTrends);
	RUN_TEST(testCoolingPressureRampsAndRespondsToAirflowFaults);
	RUN_TEST(testBlowerFaultPressureSwitchResetsAfterPressureRecovers);
	RUN_TEST(testChargeFaultsShiftSubcoolingAndHeadPressure);
	RUN_TEST(testPublishedOutputsStayWithinGaugeBounds);
	UNITY_END();
}

void loop() {}
