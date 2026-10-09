import os
import json
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import AsyncMock, patch

from fastapi import HTTPException, Request, Response


_test_data = tempfile.TemporaryDirectory()
os.environ["USERS_DB_FILE"] = str(Path(_test_data.name) / "users.json")
sys.path.insert(0, str(Path(__file__).parent))
import main
import manage_users


class AtomicJsonPersistenceTests(unittest.TestCase):
    def test_new_users_database_has_no_seeded_accounts(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "new-users.json"
            with patch.object(main, "USERS_DB_FILE", str(path)):
                self.assertEqual(main.load_users_db(), {})

            self.assertEqual(json.loads(path.read_text(encoding="utf-8")), {})

    def test_existing_users_are_loaded_without_inserting_seeded_accounts(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "existing-users.json"
            original = {"lab-student": {"pw_hash": "hash", "role": "student"}}
            path.write_text(json.dumps(original), encoding="utf-8")

            with patch.object(main, "USERS_DB_FILE", str(path)):
                self.assertEqual(main.load_users_db(), original)

            self.assertEqual(json.loads(path.read_text(encoding="utf-8")), original)

    def test_atomic_write_replaces_file_with_complete_json(self):
        path = Path(_test_data.name) / "atomic.json"

        main.write_json_atomically(str(path), {"generation": 2})

        self.assertEqual(
            json.loads(path.read_text(encoding="utf-8")), {"generation": 2}
        )

    def test_failed_replace_preserves_previous_file_and_removes_temporary_file(self):
        path = Path(_test_data.name) / "atomic-failure.json"
        path.write_text('{"generation": 1}', encoding="utf-8")

        with patch.object(main.os, "replace", side_effect=OSError("replace failed")):
            with self.assertRaises(OSError):
                main.write_json_atomically(str(path), {"generation": 2})

        self.assertEqual(
            json.loads(path.read_text(encoding="utf-8")), {"generation": 1}
        )
        self.assertEqual(list(path.parent.glob(f".{path.name}.*.tmp")), [])

    def test_corrupt_users_file_is_preserved_and_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "users.json"
            original = b"{invalid users json"
            path.write_bytes(original)

            with patch.object(main, "USERS_DB_FILE", str(path)):
                with self.assertRaisesRegex(
                    RuntimeError, "Unable to load users database"
                ):
                    main.load_users_db()

            self.assertEqual(path.read_bytes(), original)

    def test_corrupt_edges_file_is_preserved_and_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "edges.json"
            original = b"{invalid edges json"
            path.write_bytes(original)

            with patch.object(main, "EDGES_DB_FILE", str(path)):
                with self.assertRaisesRegex(
                    RuntimeError, "Unable to load edges database"
                ):
                    main.load_edges_db()

            self.assertEqual(path.read_bytes(), original)


class BackgroundTaskLifecycleTests(unittest.IsolatedAsyncioTestCase):
    async def test_shutdown_cancels_simulation_and_mqtt_tasks(self):
        with (
            patch.object(main, "load_edges_db", return_value={}),
            patch.object(main, "start_mdns_advertisement"),
            patch.object(main, "stop_mdns_advertisement"),
        ):
            await main.startup_event()
            simulation_task = main.simulation_task
            mqtt_task = main.mqtt_task

            self.assertIsNotNone(simulation_task)
            self.assertIsNotNone(mqtt_task)

            await main.shutdown_event()

        self.assertTrue(simulation_task.cancelled())
        self.assertTrue(mqtt_task.cancelled())
        self.assertIsNone(main.simulation_task)
        self.assertIsNone(main.mqtt_task)


class UserProvisioningTests(unittest.TestCase):
    def test_bootstrap_admin_creates_hashed_account(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "users.json"
            with patch.object(main, "USERS_DB_FILE", str(path)):
                manage_users.bootstrap_admin(
                    "initial-admin", "A sufficiently long password 123!"
                )
                users = main.load_users_db()

        self.assertEqual(users["initial-admin"]["role"], "admin")
        self.assertNotIn("pw", users["initial-admin"])
        self.assertTrue(
            main.verify_password(
                "A sufficiently long password 123!", users["initial-admin"]
            )
        )

    def test_password_rotation_hashes_legacy_password_and_preserves_role(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "users.json"
            path.write_text(
                json.dumps({"admin": {"pw": "admin", "role": "instructor"}}),
                encoding="utf-8",
            )
            with patch.object(main, "USERS_DB_FILE", str(path)):
                manage_users.set_password("admin", "A sufficiently long password 123!")
                users = main.load_users_db()

        self.assertEqual(users["admin"]["role"], "instructor")
        self.assertNotIn("pw", users["admin"])
        self.assertTrue(
            main.verify_password("A sufficiently long password 123!", users["admin"])
        )


class CorsConfigurationTests(unittest.TestCase):
    def test_cors_uses_explicit_local_origins(self):
        cors_middleware = next(
            middleware
            for middleware in main.app.user_middleware
            if middleware.cls is main.CORSMiddleware
        )

        self.assertEqual(
            set(cors_middleware.kwargs["allow_origins"]),
            {
                "http://localhost",
                "http://127.0.0.1",
                "http://localhost:8080",
                "http://127.0.0.1:8080",
            },
        )
        self.assertTrue(cors_middleware.kwargs["allow_credentials"])


class InstructorToggleSyncTests(unittest.IsolatedAsyncioTestCase):
    async def test_fault_toggle_is_persisted_and_published_to_selected_trainer(self):
        original_state = main.state
        main.state = main.AppState()
        edge_id = "trainer-toggle-test"
        main.state.selected_edge_id = edge_id
        main.state.edges[edge_id] = {
            "edge_id": edge_id,
            "label": "Toggle Test Trainer",
            "last_seen": time.time(),
            "runtime": main.default_runtime(),
        }
        publish_command = AsyncMock(return_value=True)

        try:
            with (
                patch.object(main, "require_instructor_or_admin"),
                patch.object(main, "maybe_select_edge"),
                patch.object(main, "save_edges_db"),
                patch.object(
                    main,
                    "publish_selected_trainer_command",
                    new=publish_command,
                ),
            ):
                response = await main.toggle_state(
                    make_request(), id="f24", state_value=1, edge_id=edge_id
                )

            self.assertTrue(main.state.fault_active[24])
            self.assertTrue(main.state.edges[edge_id]["runtime"]["fault_active"][24])
            publish_command.assert_awaited_once_with(
                {"action": "toggle", "id": "f24", "state": 1}
            )
            self.assertTrue(response["trainer_synced"])
        finally:
            main.state = original_state


def make_request(headers=()):
    return Request(
        {
            "type": "http",
            "asgi": {"version": "3.0"},
            "http_version": "1.1",
            "method": "POST",
            "scheme": "http",
            "path": "/api/test",
            "raw_path": b"/api/test",
            "query_string": b"",
            "headers": list(headers),
            "client": ("test", 1),
            "server": ("test", 80),
        }
    )


class SessionAuthorizationTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        main.sessions.clear()
        main.users_db.clear()
        main.users_db.update(
            {
                "test-instructor": {
                    "pw_hash": main.hash_password("instructor-pass"),
                    "role": "instructor",
                },
                "test-student": {
                    "pw_hash": main.hash_password("student-pass"),
                    "role": "student",
                },
            }
        )

    async def login(self, username, password):
        return await main.login(make_request(), Response(), username, password)

    async def test_login_issues_distinct_sessions(self):
        instructor = await self.login("test-instructor", "instructor-pass")
        student = await self.login("test-student", "student-pass")

        self.assertNotEqual(instructor["token"], student["token"])
        self.assertEqual(
            main.get_session_role(
                make_request(
                    [(b"authorization", f"Bearer {instructor['token']}".encode())]
                )
            ),
            "instructor",
        )
        self.assertEqual(
            main.get_session_role(
                make_request(
                    [
                        (
                            b"cookie",
                            f"{main.SESSION_COOKIE_NAME}={student['token']}".encode(),
                        )
                    ]
                )
            ),
            "student",
        )

    async def test_instructor_controls_reject_students_and_anonymous_clients(self):
        instructor = await self.login("test-instructor", "instructor-pass")
        student = await self.login("test-student", "student-pass")
        instructor_request = make_request(
            [(b"authorization", f"Bearer {instructor['token']}".encode())]
        )
        student_request = make_request(
            [(b"authorization", f"Bearer {student['token']}".encode())]
        )

        main.require_instructor_or_admin(instructor_request)
        with self.assertRaises(HTTPException) as student_error:
            main.require_instructor_or_admin(student_request)
        self.assertEqual(student_error.exception.status_code, 401)
        with self.assertRaises(HTTPException) as anonymous_error:
            main.require_instructor_or_admin(make_request())
        self.assertEqual(anonymous_error.exception.status_code, 401)

    async def test_expired_and_logged_out_sessions_are_rejected(self):
        login = await self.login("test-instructor", "instructor-pass")
        token = login["token"]
        request = make_request([(b"authorization", f"Bearer {token}".encode())])
        main.sessions[token]["expires_at"] = time.time() - 1
        self.assertIsNone(main.get_session_role(request))

        login = await self.login("test-instructor", "instructor-pass")
        token = login["token"]
        request = make_request([(b"authorization", f"Bearer {token}".encode())])
        await main.logout(request)
        self.assertIsNone(main.get_session_role(request))



class UserManagementApiTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        main.sessions.clear()
        main.users_db.clear()
        main.users_db.update(
            {
                "boss": {"pw_hash": main.hash_password("boss-password"), "role": "instructor"},
                "pupil": {"pw_hash": main.hash_password("pupil-password"), "role": "student"},
            }
        )
        self.save = patch.object(main, "save_users_db")
        self.save.start()
        self.addCleanup(self.save.stop)

    async def session(self, username, password):
        login = await main.login(make_request(), Response(), username, password)
        return make_request([(b"authorization", f"Bearer {login['token']}".encode())])

    async def test_list_exposes_fingerprint_but_never_the_full_hash(self):
        request = await self.session("boss", "boss-password")
        payload = await main.list_users(request)
        self.assertEqual([u["user"] for u in payload["users"]], ["boss", "pupil"])
        self.assertTrue(payload["users"][0]["hash_scheme"].startswith("pbkdf2_sha256/"))
        self.assertNotIn(main.users_db["boss"]["pw_hash"], json.dumps(payload))

    async def test_students_cannot_manage_users(self):
        request = await self.session("pupil", "pupil-password")
        with self.assertRaises(HTTPException):
            await main.list_users(request)
        with self.assertRaises(HTTPException):
            await main.delete_user_account(request, "boss")

    async def test_reset_password_changes_hash_and_revokes_sessions(self):
        request = await self.session("boss", "boss-password")
        await self.session("pupil", "pupil-password")
        await main.reset_user_password(request, "pupil", "brand-new-pass")
        self.assertTrue(main.verify_password("brand-new-pass", main.users_db["pupil"]))
        self.assertFalse(main.verify_password("pupil-password", main.users_db["pupil"]))
        self.assertFalse(any(s["username"] == "pupil" for s in main.sessions.values()))

    async def test_delete_removes_user_but_not_self_or_last_admin(self):
        request = await self.session("boss", "boss-password")
        await main.delete_user_account(request, "pupil")
        self.assertNotIn("pupil", main.users_db)
        with self.assertRaises(HTTPException) as own:
            await main.delete_user_account(request, "boss")
        self.assertEqual(own.exception.status_code, 400)
        with self.assertRaises(HTTPException) as missing:
            await main.delete_user_account(request, "nobody")
        self.assertEqual(missing.exception.status_code, 404)

    def test_cli_listing_and_export_include_hashes_only_when_requested(self):
        with tempfile.TemporaryDirectory() as directory:
            users_path = Path(directory) / "users.json"
            export_path = Path(directory) / "backup.json"
            users_path.write_text(json.dumps(main.users_db), encoding="utf-8")
            with patch.object(main, "USERS_DB_FILE", str(users_path)):
                plain = "\n".join(manage_users.list_users())
                full = "\n".join(manage_users.list_users(show_hashes=True))
                self.assertEqual(manage_users.export_users(str(export_path)), 2)
            self.assertNotIn(main.users_db["boss"]["pw_hash"], plain)
            self.assertIn(main.users_db["boss"]["pw_hash"], full)
            exported = json.loads(export_path.read_text(encoding="utf-8"))
            self.assertEqual(exported["pupil"]["pw_hash"], main.users_db["pupil"]["pw_hash"])

class TrainerStudentDataSyncTests(unittest.IsolatedAsyncioTestCase):
    async def test_existing_trainer_heartbeat_does_not_save_edges_to_disk(self):
        original_state = main.state
        main.state = main.AppState()
        edge_id = "trainer-heartbeat-test"
        main.state.selected_edge_id = edge_id
        main.state.edges[edge_id] = {
            "edge_id": edge_id,
            "label": "Heartbeat Test Trainer",
            "last_seen": time.time(),
            "runtime": main.default_runtime(),
        }

        try:
            with patch.object(main, "save_edges_db") as save_edges:
                response = await main.edge_heartbeat(
                    {
                        "edge_id": edge_id,
                        "device_name": "Heartbeat Test Trainer",
                        "trainer_type": "straight_ac_furnace",
                        "wifi_rssi": -60,
                    },
                    make_request(),
                )

            self.assertEqual(response["edge_id"], edge_id)
            save_edges.assert_not_called()
        finally:
            main.state = original_state

    async def test_heartbeat_updates_teacher_student_status(self):
        original_edges = main.state.edges
        original_selected_edge_id = main.state.selected_edge_id
        original_student_score = main.state.student_score
        original_latest_diagnosis = main.state.latest_diagnosis
        original_work_history_log = main.state.work_history_log

        main.state.edges = {}
        main.state.selected_edge_id = ""
        try:
            with patch.object(main, "save_edges_db"):
                await main.edge_heartbeat(
                    {
                        "edge_id": "trainer-sync-test",
                        "device_name": "Trainer Sync Test",
                        "trainer_type": "straight_ac_furnace",
                        "student_score": 72,
                        "diagnosis": "INCORRECT: test answer",
                        "work_history": "Test student history",
                        "od_low_press": 126.0,
                        "od_suction_temp": 74.8,
                    },
                    make_request(),
                )
                status = await main.get_status()

            self.assertEqual(status["student_score"], 72)
            self.assertEqual(status["diagnosis"], "INCORRECT: test answer")
            self.assertEqual(status["work_history"], "Test student history")
            self.assertEqual(status["od_low_press"], 126.0)
            self.assertEqual(status["od_suction_temp"], 74.8)
        finally:
            main.state.edges = original_edges
            main.state.selected_edge_id = original_selected_edge_id
            main.state.student_score = original_student_score
            main.state.latest_diagnosis = original_latest_diagnosis
            main.state.work_history_log = original_work_history_log


class DiagnosisSubmissionTests(unittest.IsolatedAsyncioTestCase):
    async def submit_without_edge_id(self, diagnosis):
        original_state = main.state
        original_mqtt_client = main.mqtt_client
        main.state = main.AppState()
        main.mqtt_client = None
        edge_id = "trainer-diagnosis-test"
        main.state.selected_edge_id = edge_id
        main.state.edges[edge_id] = {
            "edge_id": edge_id,
            "label": "Diagnosis Test Trainer",
            "last_seen": time.time(),
            "diagnosis": "None",
            "student_score": 100,
            "work_history": "",
            "runtime": main.default_runtime(),
        }

        try:
            with (
                patch.object(main, "save_edges_db"),
                patch.object(
                    main, "publish_selected_trainer_command", new_callable=AsyncMock
                ),
            ):
                response = await main.submit_diagnosis(
                    make_request(), diagnosis=diagnosis, edge_id=None
                )
                await main.edge_heartbeat(
                    {
                        "edge_id": edge_id,
                        "device_name": "Diagnosis Test Trainer",
                        "trainer_type": "straight_ac_furnace",
                        "student_score": 100,
                        "diagnosis": "None",
                        "work_history": "",
                    },
                    make_request(),
                )
                status = await main.get_status()
            return response, main.state.edges[edge_id], status
        finally:
            main.state = original_state
            main.mqtt_client = original_mqtt_client

    async def test_submission_without_edge_id_updates_selected_trainer(self):
        diagnosis = "Failed Compressor / Overload"

        response, edge, status = await self.submit_without_edge_id(diagnosis)

        self.assertEqual(response.body, b"INCORRECT")
        self.assertEqual(edge["diagnosis"], f"INCORRECT: {diagnosis}")
        self.assertEqual(edge["student_score"], 90)
        self.assertIn("INCORRECT", edge["work_history"])
        self.assertEqual(status["diagnosis"], f"INCORRECT: {diagnosis}")
        self.assertEqual(status["student_score"], 90)

    async def test_correct_answer_remains_visible_after_fault_reset(self):
        response, edge, status = await self.submit_without_edge_id("Normal Operation")

        self.assertEqual(response.body, b"CORRECT")
        self.assertEqual(edge["diagnosis"], "CORRECT: Normal Operation")
        self.assertEqual(edge["student_score"], 100)
        self.assertIn("CORRECT", edge["work_history"])
        self.assertEqual(status["diagnosis"], "CORRECT: Normal Operation")


class SimulationAirTemperatureTests(unittest.IsolatedAsyncioTestCase):
    async def run_one_tick(
        self,
        *,
        initial_temp,
        y_call=False,
        w_call=False,
        furnace_heating=False,
        blower_on=False,
    ):
        original_state = main.state
        main.state = main.AppState()
        main.state.sim_id_supply_temp = initial_temp
        main.state.state_y = y_call
        main.state.state_w = w_call
        main.state.furnace_state = (
            "FURNACE_HEATING" if furnace_heating else "FURNACE_IDLE"
        )
        main.state.heat_blower_on = blower_on

        class EndSimulationTick(Exception):
            pass

        try:
            with (
                patch.object(main.time, "monotonic", side_effect=[10.0, 10.2]),
                patch.object(main.asyncio, "sleep", side_effect=EndSimulationTick),
            ):
                with self.assertRaises(EndSimulationTick):
                    await main.simulation_loop()
            return main.state.sim_id_supply_temp
        finally:
            main.state = original_state

    async def test_supply_air_moves_toward_cooling_target_gradually(self):
        supply_temp = await self.run_one_tick(initial_temp=75.0, y_call=True)

        self.assertGreater(supply_temp, 55.0)
        self.assertLess(supply_temp, 75.0)

    async def test_supply_air_moves_toward_heating_target_gradually(self):
        supply_temp = await self.run_one_tick(
            initial_temp=75.0,
            w_call=True,
            furnace_heating=True,
            blower_on=True,
        )

        self.assertGreater(supply_temp, 75.0)
        self.assertLess(supply_temp, 120.0)

    async def test_supply_air_recovers_toward_return_temp_without_airflow(self):
        supply_temp = await self.run_one_tick(initial_temp=60.0)

        self.assertGreater(supply_temp, 60.0)
        self.assertLess(supply_temp, 75.0)


class TrainerSettingsReconciliationTests(unittest.IsolatedAsyncioTestCase):
    async def test_selected_trainer_receives_engine_settings_when_they_differ(self):
        original_edges = main.state.edges
        original_selected_edge_id = main.state.selected_edge_id
        original_settings = (
            main.state.set_od_temp,
            main.state.set_id_temp,
            main.state.set_rh,
            main.state.current_refrigerant,
            main.state.id_is_txv,
            main.state.od_is_txv,
            main.state.is_b_type,
        )
        mqtt_mock = AsyncMock()
        original_mqtt_client = main.mqtt_client

        main.state.edges = {
            "trainer-settings-test": {
                "edge_id": "trainer-settings-test",
                "label": "Trainer Settings Test",
                "last_seen": time.time(),
                "trainer_type": "ac_gas",
                "runtime": {
                    "set_od_temp": 90.0,
                    "set_id_temp": 79.0,
                    "set_rh": 50.0,
                    "current_refrigerant": "R410A",
                    "id_is_txv": True,
                    "od_is_txv": True,
                    "is_b_type": False,
                },
            }
        }
        main.state.selected_edge_id = "trainer-settings-test"
        main.state.set_od_temp = 90.0
        main.state.set_id_temp = 79.0
        main.state.set_rh = 50.0
        main.state.current_refrigerant = "R410A"
        main.state.id_is_txv = True
        main.state.od_is_txv = True
        main.state.is_b_type = False
        main.mqtt_client = mqtt_mock
        try:
            with patch.object(main, "save_edges_db"):
                await main.edge_heartbeat(
                    {
                        "edge_id": "trainer-settings-test",
                        "device_name": "Trainer Settings Test",
                        "trainer_type": "straight_ac_furnace",
                        "set_od": 90.0,
                        "set_id": 75.0,
                        "set_rh": 50.0,
                        "refrigerant": "R410A",
                        "id_is_txv": 1,
                        "od_is_txv": 1,
                        "is_b_type": 0,
                    },
                    make_request(),
                )

            mqtt_mock.publish.assert_awaited_once()
            topic = mqtt_mock.publish.await_args.args[0]
            command = json.loads(mqtt_mock.publish.await_args.args[1])
            self.assertEqual(topic, "trainer/trainer-settings-test/command")
            self.assertEqual(command["action"], "set_settings")
            self.assertEqual(command["id"], 79.0)
        finally:
            main.state.edges = original_edges
            main.state.selected_edge_id = original_selected_edge_id
            (
                main.state.set_od_temp,
                main.state.set_id_temp,
                main.state.set_rh,
                main.state.current_refrigerant,
                main.state.id_is_txv,
                main.state.od_is_txv,
                main.state.is_b_type,
            ) = original_settings
            main.mqtt_client = original_mqtt_client


if __name__ == "__main__":
    unittest.main()
