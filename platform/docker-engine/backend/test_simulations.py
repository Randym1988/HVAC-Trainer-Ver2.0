import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


_test_data = tempfile.TemporaryDirectory()
os.environ["USERS_DB_FILE"] = str(Path(_test_data.name) / "users.json")
sys.path.insert(0, str(Path(__file__).parent))
import main


class HeatPumpModeSimulationTests(unittest.TestCase):
    def get_modes(
        self,
        *,
        is_b_type: bool,
        o_call: bool,
        o_wire_broken: bool = False,
        reversing_valve_failed: bool = False,
    ) -> tuple[bool, bool]:
        return main.get_hvac_mode_states(
            trainer_type="heat_pump",
            compressor_running=True,
            effective_w_call=False,
            o_call=o_call,
            is_b_type=is_b_type,
            o_wire_broken=o_wire_broken,
            reversing_valve_failed=reversing_valve_failed,
            flame_active=False,
        )

    def test_o_type_reversing_valve_selects_heat_or_cooling(self):
        self.assertEqual(self.get_modes(is_b_type=False, o_call=False), (True, False))
        self.assertEqual(self.get_modes(is_b_type=False, o_call=True), (False, True))

    def test_b_type_reversing_valve_selects_heat_or_cooling(self):
        self.assertEqual(self.get_modes(is_b_type=True, o_call=False), (False, True))
        self.assertEqual(self.get_modes(is_b_type=True, o_call=True), (True, False))

    def test_valve_fault_and_broken_o_wire_change_mode_only_when_o_is_called(self):
        self.assertEqual(
            self.get_modes(
                is_b_type=False, o_call=True, reversing_valve_failed=True
            ),
            (True, False),
        )
        self.assertEqual(
            self.get_modes(is_b_type=True, o_call=True, o_wire_broken=True),
            (False, True),
        )
        self.assertEqual(
            self.get_modes(is_b_type=False, o_call=False, reversing_valve_failed=True),
            (True, False),
        )

    def test_furnace_flame_telemetry_does_not_use_heat_pump_valve_state(self):
        self.assertEqual(
            main.get_hvac_mode_states(
                trainer_type="ac_gas",
                compressor_running=True,
                effective_w_call=False,
                o_call=False,
                is_b_type=False,
                o_wire_broken=False,
                reversing_valve_failed=False,
                flame_active=True,
            ),
            (True, True),
        )


class SimulationDiagnosisTests(unittest.TestCase):
    def test_every_portal_simulation_has_a_backend_diagnosis(self):
        expected_by_simulation = {
            1: "Failed Indoor Blower",
            2: "Failed Indoor Blower",
            3: "Failed Condenser Fan",
            4: "Failed Condenser Fan",
            5: "Stuck Reversing Valve",
            6: "Failed Indoor Blower",
            7: "Tripped Safety Limit",
            8: "Defective Sequencer",
            9: "Defective Sequencer",
            10: "Defective Sequencer",
            11: "Defective Sequencer",
            12: "Failed Heat Strip Element",
            13: "Stuck Reversing Valve",
            14: "Broken Y Wire",
            15: "Failed Compressor / Overload",
        }

        for simulation_id, expected in expected_by_simulation.items():
            with self.subTest(simulation_id=simulation_id):
                fault_active = [False] * 57
                sim_active = [False] * 16
                sim_active[simulation_id] = True
                with (
                    patch.object(main.state, "fault_active", fault_active),
                    patch.object(main.state, "sim_active", sim_active),
                ):
                    self.assertEqual(main.get_expected_diagnosis(), expected)


if __name__ == "__main__":
    unittest.main()
