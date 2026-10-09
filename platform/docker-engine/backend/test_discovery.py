import json
import socket
import unittest

from discovery_service import DISCOVERY_REQUEST, DiscoveryService


def ask(port: int, payload: bytes, timeout: float = 1.0) -> bytes | None:
    client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    client.settimeout(timeout)
    try:
        client.sendto(payload, ("127.0.0.1", port))
        return client.recvfrom(1024)[0]
    except socket.timeout:
        return None
    finally:
        client.close()


class DiscoveryServiceTests(unittest.TestCase):
    def setUp(self):
        self.service = DiscoveryService(
            port=0, http_port=8000, mqtt_port=1883, hostname="lab-trainer"
        )
        self.service.start(wait_seconds=3)
        self.addCleanup(self.service.stop)

    def test_replies_to_discovery_ping_with_service_details(self):
        reply = ask(self.service.bound_port, DISCOVERY_REQUEST)
        self.assertIsNotNone(reply)
        self.assertEqual(
            json.loads(reply),
            {
                "service": "HVAC_HIL_TRAINER",
                "version": "1.0.0",
                "http_port": 8000,
                "mqtt_port": 1883,
                "hostname": "lab-trainer",
            },
        )

    def test_ignores_unrelated_payloads(self):
        self.assertIsNone(ask(self.service.bound_port, b"HELLO", timeout=0.4))

    def test_rapid_repeat_requests_from_one_sender_are_throttled(self):
        client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        client.settimeout(0.4)
        self.addCleanup(client.close)
        for _ in range(3):
            client.sendto(DISCOVERY_REQUEST, ("127.0.0.1", self.service.bound_port))
        client.recvfrom(1024)
        with self.assertRaises(socket.timeout):
            client.recvfrom(1024)

    def test_stop_ends_the_listener_thread(self):
        self.service.stop()
        self.assertFalse(
            any(t.name == "udp-discovery" and t.is_alive() for t in __import__("threading").enumerate())
        )


if __name__ == "__main__":
    unittest.main()
