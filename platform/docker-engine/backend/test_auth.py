import os
import json
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import patch

from fastapi import HTTPException, Request, Response


_test_data = tempfile.TemporaryDirectory()
os.environ["USERS_DB_FILE"] = str(Path(_test_data.name) / "users.json")
sys.path.insert(0, str(Path(__file__).parent))
import main


class AtomicJsonPersistenceTests(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
