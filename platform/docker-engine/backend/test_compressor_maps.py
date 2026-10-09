import re
import unittest
from pathlib import Path

import compressor_maps
from compressor_maps import (
    YA31_ELECTRICAL,
    YA31_EVAPORATING_TEMPS_F,
    YA31_ROWS,
    YP31_ELECTRICAL,
    YP31_EVAPORATING_TEMPS_F,
    YP31_ROWS,
    ZP29_ELECTRICAL,
    ZP29_EVAPORATING_TEMPS_F,
    ZP29_ROWS,
    compressor_electrical_reading,
    compressor_model_name,
    evaluate_compressor,
)

FIRMWARE_PHYSICS = (
    Path(__file__).resolve().parents[3]
    / "trainers"
    / "unified-master"
    / "firmware"
    / "src"
    / "PhysicsEngine.cpp"
)


class Ya31CompressorMapTests(unittest.TestCase):
    def assertPoint(self, point, capacity, power, amps, mass):
        self.assertAlmostEqual(point.capacity_btu_per_hour, capacity, places=1)
        self.assertAlmostEqual(point.power_watts, power, places=1)
        self.assertAlmostEqual(point.current_amps, amps, places=3)
        self.assertAlmostEqual(point.mass_flow_lb_per_hour, mass, places=1)

    def test_published_points(self):
        self.assertPoint(evaluate_compressor("R454B", 35.0, 110.0), 29800, 2240, 10.1, 322)
        self.assertPoint(evaluate_compressor("R454B", 55.0, 140.0), 35700, 3320, 14.8, 447)
        self.assertPoint(evaluate_compressor("R454B", -10.0, 50.0), 17150, 1135, 5.4, 152)
        self.assertPoint(evaluate_compressor("r454b", 77.0, 120.0), 61300, 2320, 10.4, 672)

    def test_bilinear_interpolation(self):
        self.assertPoint(evaluate_compressor("R454B", 45.0, 115.0), 35675, 2362.5, 10.625, 390.5)

    def test_amps_rise_with_head_and_clamp_outside_chart(self):
        low = evaluate_compressor("R454B", 45.0, 100.0).current_amps
        high = evaluate_compressor("R454B", 45.0, 140.0).current_amps
        self.assertGreater(high, low)
        self.assertEqual(evaluate_compressor("R454B", 45.0, 170.0), evaluate_compressor("R454B", 45.0, 145.0))

    def test_each_compressor_has_its_own_chart(self):
        self.assertIsNone(evaluate_compressor("R22", 45.0, 115.0))
        self.assertIsNone(compressor_model_name("R22"))
        self.assertIn("ZP29K6E-PFV", compressor_model_name("R410A"))
        self.assertIn("YA31K1E-PFV", compressor_model_name("R454B"))
        self.assertIn("YP31K1T-PFV", compressor_model_name("r32"))

    def test_zp29_points_match_firmware_tests(self):
        self.assertPoint(evaluate_compressor("R410A", 45.0, 130.0), 29400, 2790, 12.3, 433)
        self.assertPoint(evaluate_compressor("R410A", 42.5, 122.5), 31325, 2542.5, 11.175, 437.75)

    @unittest.skipUnless(FIRMWARE_PHYSICS.exists(), "firmware source not available")
    def test_tables_match_firmware(self):
        source = FIRMWARE_PHYSICS.read_text(encoding="utf-8")
        for prefix, py_evaps, py_rows in (
            ("k", ZP29_EVAPORATING_TEMPS_F, ZP29_ROWS),
            ("kYa31", YA31_EVAPORATING_TEMPS_F, YA31_ROWS),
            ("kYp31", YP31_EVAPORATING_TEMPS_F, YP31_ROWS),
        ):
            with self.subTest(table=prefix):
                evaps = re.search(prefix + r"EvaporatingTemperaturesF\[9\] = \{(.*?)\};", source, re.S).group(1)
                self.assertEqual(tuple(float(v) for v in re.findall(r"(-?[\d.]+)f", evaps)), py_evaps)
                block = re.search(prefix + r"CompressorMap\[\] = \{(.*?)\n\};", source, re.S).group(1)
                rows = re.findall(r"\{([\d.]+)f, (\d+), (\d+), \{(.*?)\}\},", block, re.S)
                self.assertEqual(len(rows), len(py_rows))
                for (cond, first, count, body), (py_cond, py_first, py_points) in zip(rows, py_rows):
                    points = [
                        tuple(float(v) for v in p)
                        for p in re.findall(r"\{([\d.]+)f, ([\d.]+)f, ([\d.]+)f, ([\d.]+)f\}", body)
                    ]
                    self.assertEqual(
                        (float(cond), int(first), int(count)), (py_cond, py_first, len(py_points))
                    )
                    self.assertLessEqual(py_first + len(py_points), len(py_evaps))
                    self.assertEqual(points, [tuple(float(v) for v in p) for p in py_points])


class Yp31CompressorMapTests(unittest.TestCase):
    assertPoint = Ya31CompressorMapTests.assertPoint

    def test_published_points(self):
        self.assertPoint(evaluate_compressor("R32", 45.0, 130.0), 31600, 2960, 13.3, 304)
        self.assertPoint(evaluate_compressor("R32", -10.0, 90.0), 11600, 1845, 8.4, 98)
        self.assertPoint(evaluate_compressor("R32", 10.0, 120.0), 14550, 2710, 12.1, 137)
        self.assertPoint(evaluate_compressor("R32", 55.0, 145.0), 33900, 3610, 16.2, 353)
        self.assertPoint(evaluate_compressor("R32", 77.0, 140.0), 53600, 3160, 14.1, 539)

    def test_bilinear_interpolation(self):
        self.assertPoint(evaluate_compressor("R32", 45.0, 115.0), 35150, 2405, 10.9, 315.5)

    def test_amps_rise_with_head_and_clamp_outside_chart(self):
        low = evaluate_compressor("R32", 45.0, 100.0).current_amps
        high = evaluate_compressor("R32", 45.0, 140.0).current_amps
        self.assertGreater(high, low)
        self.assertEqual(evaluate_compressor("R32", 45.0, 170.0), evaluate_compressor("R32", 45.0, 145.0))

class CompressorElectricalTests(unittest.TestCase):
    def test_spec_follows_refrigerant(self):
        reading = compressor_electrical_reading("R454B", 12.0, 18.3)
        self.assertEqual(reading["compressor_electrical_model"], "YA31K1E-PFV")
        self.assertEqual(reading["run_cap_rated_uf"], 40.0)
        self.assertEqual((reading["winding_start_ohms"], reading["winding_run_ohms"]), (1.43, 0.72))
        reading = compressor_electrical_reading("R32", 12.0, 18.3)
        self.assertEqual(reading["compressor_electrical_model"], "YP31K1T-PFV")
        self.assertEqual(reading["run_cap_rated_uf"], 40.0)
        self.assertEqual((reading["winding_start_ohms"], reading["winding_run_ohms"]), (1.43, 0.72))
        for refrigerant in ("R410A", "R22", None):
            reading = compressor_electrical_reading(refrigerant, 12.0, 18.3)
            self.assertEqual(reading["compressor_electrical_model"], "ZP29K6E-PFV")
            self.assertEqual(reading["run_cap_rated_uf"], 45.0)
            self.assertEqual((reading["winding_start_ohms"], reading["winding_run_ohms"]), (1.58, 0.92))

    def test_running_readings_match_firmware_test_values(self):
        reading = compressor_electrical_reading("R454B", 12.0, 18.3)
        self.assertEqual(reading["line_volts"], 239.7)
        self.assertEqual(reading["run_cap_volts"], 320.2)
        self.assertEqual(reading["run_cap_uf"], 39.4)
        self.assertEqual(reading["comp_start_amps"], 4.8)
        self.assertEqual(reading["comp_run_amps"], 11.9)
        reading = compressor_electrical_reading("R410A", 12.0, 18.3)
        self.assertEqual(reading["run_cap_uf"], 44.3)
        self.assertEqual(reading["comp_start_amps"], 5.4)

    def test_stopped_and_locked_rotor(self):
        stopped = compressor_electrical_reading("R454B", 0.0, 4.5)
        self.assertEqual(stopped["line_volts"], 241.1)
        self.assertEqual(
            (stopped["run_cap_volts"], stopped["run_cap_uf"], stopped["comp_start_amps"], stopped["comp_run_amps"]),
            (0.0, 0.0, 0.0, 0.0),
        )
        locked = compressor_electrical_reading("R454B", 143.0, 149.3)
        self.assertEqual(locked["line_volts"], 226.6)
        self.assertEqual(locked["run_cap_uf"], 134.9)  # start cap held in by the potential relay

    @unittest.skipUnless(FIRMWARE_PHYSICS.exists(), "firmware source not available")
    def test_constants_match_firmware(self):
        source = FIRMWARE_PHYSICS.read_text(encoding="utf-8")
        for name, spec in (
            ("kZp29Electrical", ZP29_ELECTRICAL),
            ("kYa31Electrical", YA31_ELECTRICAL),
            ("kYp31Electrical", YP31_ELECTRICAL),
        ):
            body = re.search(name + r" = \{.*?\n\t(\".*?)\n\};", source, re.S).group(1)
            fields = [f.strip().strip('"').rstrip("f") for f in body.rstrip(",").split(",")]
            self.assertEqual(fields[0], spec.model)
            self.assertEqual([float(v) for v in fields[1:8]], list(spec[1:8]))
            self.assertEqual(fields[8], spec.potential_relay)
        for cpp, py in (
            ("kNoLoadLineVolts", compressor_maps.NO_LOAD_LINE_VOLTS),
            ("kLineDropVoltsPerAmp", compressor_maps.LINE_DROP_VOLTS_PER_AMP),
            ("kLockedRotorAmpsThreshold", compressor_maps.LOCKED_ROTOR_AMPS_THRESHOLD),
            ("kRunningCapVoltsRatioBase", compressor_maps.RUNNING_CAP_VOLTS_RATIO_BASE),
            ("kRunningCapVoltsRatioPerAmp", compressor_maps.RUNNING_CAP_VOLTS_RATIO_PER_AMP),
            ("kRunningCapVoltsRatioMin", compressor_maps.RUNNING_CAP_VOLTS_RATIO_MIN),
            ("kRunningCapVoltsRatioMax", compressor_maps.RUNNING_CAP_VOLTS_RATIO_MAX),
            ("kLockedRotorCapVoltsRatio", compressor_maps.LOCKED_ROTOR_CAP_VOLTS_RATIO),
            ("kInCircuitCapFraction", compressor_maps.IN_CIRCUIT_CAP_FRACTION),
            ("kMicrofaradConstant", compressor_maps.MICROFARAD_CONSTANT),
            ("kWindingPhaseCos", compressor_maps.WINDING_PHASE_COS),
            ("kWindingPhaseSin", compressor_maps.WINDING_PHASE_SIN),
        ):
            value = re.search(r"constexpr float " + cpp + r" = (-?[\d.]+)f;", source).group(1)
            self.assertEqual(float(value), py, cpp)


if __name__ == "__main__":
    unittest.main()
