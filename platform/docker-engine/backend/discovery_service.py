"""UDP broadcast auto-discovery so the mobile app can find the engine without a typed IP."""

import json
import os
import socket
import threading
import time

DISCOVERY_PORT = int(os.getenv("DISCOVERY_PORT", "4210"))
DISCOVERY_REQUEST = b"DISCOVER_HVAC_TRAINER"
SERVICE_NAME = "HVAC_HIL_TRAINER"
SERVICE_VERSION = "1.0.0"

SOCKET_TIMEOUT_SECONDS = 1.0
REBIND_DELAY_SECONDS = 2.0
MAX_REBIND_DELAY_SECONDS = 30.0
# One reply per sender per interval keeps a noisy or spoofed client from using us as a reflector.
MIN_REPLY_INTERVAL_SECONDS = 0.5
MAX_TRACKED_SENDERS = 256


class DiscoveryService:
    def __init__(
        self,
        port: int | None = None,
        http_port: int | None = None,
        mqtt_port: int | None = None,
        hostname: str | None = None,
    ) -> None:
        self.port = DISCOVERY_PORT if port is None else port
        self.http_port = http_port or int(os.getenv("DISCOVERY_HTTP_PORT", "8000"))
        self.mqtt_port = mqtt_port or int(os.getenv("DISCOVERY_MQTT_PORT", "1883"))
        self.hostname = (
            hostname or os.getenv("DISCOVERY_HOSTNAME") or socket.gethostname()
        )
        self.bound_port = 0
        self._stop = threading.Event()
        self._ready = threading.Event()
        self._thread: threading.Thread | None = None
        self._last_reply: dict[str, float] = {}

    def build_reply(self) -> bytes:
        return json.dumps(
            {
                "service": SERVICE_NAME,
                "version": SERVICE_VERSION,
                "http_port": self.http_port,
                "mqtt_port": self.mqtt_port,
                "hostname": self.hostname,
            }
        ).encode("utf-8")

    def start(self, wait_seconds: float = 0.0) -> None:
        if self._thread and self._thread.is_alive():
            return
        self._stop.clear()
        self._ready.clear()
        self._thread = threading.Thread(
            target=self._run, name="udp-discovery", daemon=True
        )
        self._thread.start()
        if wait_seconds:
            self._ready.wait(wait_seconds)

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=SOCKET_TIMEOUT_SECONDS * 2)
        self._thread = None

    def _open_socket(self) -> socket.socket:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            sock.settimeout(SOCKET_TIMEOUT_SECONDS)
            sock.bind(("0.0.0.0", self.port))
        except Exception:
            sock.close()
            raise
        self.bound_port = sock.getsockname()[1]
        return sock

    def _allow_reply(self, sender_ip: str) -> bool:
        now = time.monotonic()
        if len(self._last_reply) >= MAX_TRACKED_SENDERS:
            cutoff = now - MIN_REPLY_INTERVAL_SECONDS
            self._last_reply = {
                ip: seen for ip, seen in self._last_reply.items() if seen > cutoff
            }
            if len(self._last_reply) >= MAX_TRACKED_SENDERS:
                return False
        previous = self._last_reply.get(sender_ip)
        if previous is not None and now - previous < MIN_REPLY_INTERVAL_SECONDS:
            return False
        self._last_reply[sender_ip] = now
        return True

    def _serve(self, sock: socket.socket) -> None:
        while not self._stop.is_set():
            try:
                data, sender = sock.recvfrom(512)
            except socket.timeout:
                continue
            if data.strip() != DISCOVERY_REQUEST or not self._allow_reply(sender[0]):
                continue
            sock.sendto(self.build_reply(), sender)

    def _run(self) -> None:
        delay = REBIND_DELAY_SECONDS
        while not self._stop.is_set():
            sock = None
            try:
                sock = self._open_socket()
                print(f"[Discovery] Listening for trainer discovery on UDP {self.bound_port}")
                delay = REBIND_DELAY_SECONDS
                self._ready.set()
                self._serve(sock)
            except Exception as error:
                # Network interface changes surface as socket errors; rebind and keep serving.
                print(f"[Discovery] Listener error ({error}); retrying in {delay:.0f}s")
                if self._stop.wait(delay):
                    break
                delay = min(delay * 2, MAX_REBIND_DELAY_SECONDS)
            finally:
                if sock is not None:
                    try:
                        sock.close()
                    except OSError:
                        pass
