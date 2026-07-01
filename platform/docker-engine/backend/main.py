import asyncio
import time
import random
import os
import socket
import json
import hmac
import hashlib
import secrets
from contextlib import suppress
from fastapi import FastAPI, Request, HTTPException, Response, Depends, WebSocket, WebSocketDisconnect, Query, Form
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel
from typing import Optional, Dict, Any, List
from zeroconf import IPVersion, ServiceInfo, Zeroconf
from aiomqtt import Client as MQTTClient, MqttError

app = FastAPI()

# Enable CORS for local dev connecting from the App or standard browser
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

active_websockets = []
# Global MQTT client placeholder (used for publishing from REST endpoints)
mqtt_client: Optional[MQTTClient] = None
mqtt_task: Optional[asyncio.Task] = None
MQTT_HOST = os.getenv("MQTT_HOST", "mqtt")
EDGE_REQUIRED = os.getenv("EDGE_REQUIRED", "true").lower() in {"1", "true", "yes", "on"}
SIMULATION_TICK_SECONDS = 0.20
WS_BROADCAST_INTERVAL_SECONDS = 0.25
MDNS_SERVICE_NAME = os.getenv("ENGINE_MDNS_NAME", "trainer-engine")
mdns = None
mdns_service_info = None
USERS_DB_FILE = os.getenv("USERS_DB_FILE", "users.json")
users_db: Dict[str, Dict[str, str]] = {}
PBKDF2_ROUNDS = int(os.getenv("USER_PASSWORD_ROUNDS", "150000"))
EDGES_DB_FILE = os.getenv("EDGES_DB_FILE", "edges.json")
EDGES_SAVE_INTERVAL_SECONDS = float(os.getenv("EDGES_SAVE_INTERVAL_SECONDS", "5"))
_last_edges_save_at = 0.0


def hash_password(password: str) -> str:
    salt = secrets.token_hex(16)
    digest = hashlib.pbkdf2_hmac(
        "sha256",
        password.encode("utf-8"),
        bytes.fromhex(salt),
        PBKDF2_ROUNDS,
    ).hex()
    return f"pbkdf2_sha256${PBKDF2_ROUNDS}${salt}${digest}"


def verify_password(submitted: str, record: Dict[str, str]) -> bool:
    encoded = str(record.get("pw_hash", ""))
    if encoded.startswith("pbkdf2_sha256$"):
        try:
            _, rounds_str, salt_hex, expected_hex = encoded.split("$", 3)
            rounds = int(rounds_str)
            computed = hashlib.pbkdf2_hmac(
                "sha256",
                submitted.encode("utf-8"),
                bytes.fromhex(salt_hex),
                rounds,
            ).hex()
            return hmac.compare_digest(computed, expected_hex)
        except Exception:
            return False

    # Legacy fallback for existing plaintext records; migrate on successful login.
    legacy_pw = str(record.get("pw", ""))
    return hmac.compare_digest(submitted, legacy_pw)


def load_users_db() -> Dict[str, Dict[str, str]]:
    defaults: Dict[str, Dict[str, str]] = {
        "admin": {"pw": "admin", "role": "instructor"},
        "student": {"pw": "student", "role": "student"},
    }

    if not os.path.exists(USERS_DB_FILE):
        with open(USERS_DB_FILE, "w", encoding="utf-8") as f:
            json.dump(defaults, f, indent=2)
        return defaults

    try:
        with open(USERS_DB_FILE, "r", encoding="utf-8") as f:
            loaded = json.load(f)
        if isinstance(loaded, dict):
            for username, meta in defaults.items():
                if username not in loaded:
                    loaded[username] = meta
            return loaded
    except Exception:
        pass

    with open(USERS_DB_FILE, "w", encoding="utf-8") as f:
        json.dump(defaults, f, indent=2)
    return defaults


def save_users_db() -> None:
    with open(USERS_DB_FILE, "w", encoding="utf-8") as f:
        json.dump(users_db, f, indent=2)


def require_instructor_or_admin() -> None:
    if state.authRole not in {"admin", "instructor"}:
        raise HTTPException(status_code=401, detail="DENIED")


def load_edges_db() -> Dict[str, Dict[str, Any]]:
    try:
        if not os.path.exists(EDGES_DB_FILE):
            return {}
        with open(EDGES_DB_FILE, "r", encoding="utf-8") as f:
            loaded = json.load(f)
        if not isinstance(loaded, dict):
            return {}

        restored: Dict[str, Dict[str, Any]] = {}
        for edge_id, edge in loaded.items():
            if not isinstance(edge, dict):
                continue
            safe_id = str(edge.get("edge_id") or edge_id).strip()
            if not safe_id:
                continue
            incoming_type = str(edge.get("trainer_type") or "ac_gas").strip().lower().replace("-", "_").replace(" ", "_")
            trainer_type = "heat_pump" if incoming_type in {"heat_pump", "heatpump", "hp"} else "ac_gas"
            restored[safe_id] = {
                "edge_id": safe_id,
                "label": str(edge.get("label") or safe_id),
                "source_ip": str(edge.get("source_ip") or ""),
                "last_seen": float(edge.get("last_seen") or 0.0),
                "trainer_type": trainer_type,
                "mac": str(edge.get("mac") or "").lower(),
                "w": bool(edge.get("w", False)),
                "y": bool(edge.get("y", False)),
                "g": bool(edge.get("g", False)),
                "phys_lps": bool(edge.get("phys_lps", False)),
                "phys_hps": bool(edge.get("phys_hps", False)),
                "wifi_rssi": edge.get("wifi_rssi", -55),
                "ram": edge.get("ram", 245760),
                "uptime": edge.get("uptime", "00:00:00"),
                "temp": edge.get("temp", 98.0),
                "identity_reboot_required": bool(edge.get("identity_reboot_required", False)),
                "telemetry": {},
                "runtime": {},
            }
        return restored
    except Exception:
        return {}


def save_edges_db(force: bool = False) -> None:
    global _last_edges_save_at
    now = time.time()
    if not force and (now - _last_edges_save_at) < EDGES_SAVE_INTERVAL_SECONDS:
        return

    serializable: Dict[str, Dict[str, Any]] = {}
    for edge_id, edge in state.edges.items():
        if not isinstance(edge, dict):
            continue
        serializable[edge_id] = {
            "edge_id": edge_id,
            "label": edge.get("label", edge_id),
            "source_ip": edge.get("source_ip", ""),
            "last_seen": edge.get("last_seen", 0.0),
            "trainer_type": edge.get("trainer_type", "ac_gas"),
            "mac": str(edge.get("mac", "")).lower(),
            "w": bool(edge.get("w", False)),
            "y": bool(edge.get("y", False)),
            "g": bool(edge.get("g", False)),
            "phys_lps": bool(edge.get("phys_lps", False)),
            "phys_hps": bool(edge.get("phys_hps", False)),
            "wifi_rssi": edge.get("wifi_rssi", -55),
            "ram": edge.get("ram", 245760),
            "uptime": edge.get("uptime", "00:00:00"),
            "temp": edge.get("temp", 98.0),
            "identity_reboot_required": bool(edge.get("identity_reboot_required", False)),
        }

    try:
        with open(EDGES_DB_FILE, "w", encoding="utf-8") as f:
            json.dump(serializable, f, indent=2)
        _last_edges_save_at = now
    except Exception:
        pass


def resolve_host_ip() -> str:
    candidates = ["host.docker.internal", "gateway.docker.internal"]
    for host in candidates:
        with suppress(Exception):
            resolved = socket.gethostbyname(host)
            if resolved and resolved != "127.0.0.1":
                return resolved
    return ""


def start_mdns_advertisement() -> None:
    global mdns, mdns_service_info
    host_ip = resolve_host_ip()
    if not host_ip:
        print("[mDNS] Host IP not resolved; skipping advertisement")
        return

    mdns = Zeroconf(ip_version=IPVersion.V4Only)
    service_type = "_http._tcp.local."
    service_name = f"{MDNS_SERVICE_NAME}._http._tcp.local."
    mdns_service_info = ServiceInfo(
        service_type,
        service_name,
        addresses=[socket.inet_aton(host_ip)],
        port=8000,
        properties={b"path": b"/"},
        server=f"{MDNS_SERVICE_NAME}.local.",
    )
    mdns.register_service(mdns_service_info)
    print(f"[mDNS] Advertising {MDNS_SERVICE_NAME}.local -> http://{host_ip}:8000")


def stop_mdns_advertisement() -> None:
    global mdns, mdns_service_info
    if mdns and mdns_service_info:
        with suppress(Exception):
            mdns.unregister_service(mdns_service_info)
    if mdns:
        with suppress(Exception):
            mdns.close()
    mdns = None
    mdns_service_info = None

# ==========================================
# GLOBAL STATE (Replaces C++ variables)
# ==========================================
class AppState:
    def __init__(self):
        self.edge_connected = False
        self.edge_last_seen = 0.0
        self.edge_timeout_seconds = 8.0
        self.ws_last_broadcast = 0.0
        self.trainer_type = "ac_gas"
        self.selected_edge_id: Optional[str] = None
        self.edges: Dict[str, Dict[str, Any]] = {}

        # Timers & States
        self.furnace_state = "FURNACE_IDLE"
        self.furnace_timer = 0
        self.blower_on_delay = 0
        self.blower_off_delay = 0

        self.inducer_on = False
        self.igniter_on = False
        self.gas_valve_on = False
        self.heat_blower_on = False

        self.last_w_state = False
        self.last_y_state = False
        self.last_g_state = False
        self.last_o_state = False

        self.state_w = False
        self.state_y = False
        self.state_g = False
        self.state_o = False
        self.is_b_type = False
        self.force_defrost = False

        # Telemetry
        self.sim_comp_amps = 0.0
        self.sim_od_fan_amps = 0.0
        self.sim_id_fan_amps = 0.0
        self.comp_start_time = 0
        self.last_comp_state = False

        self.phys_lps_tripped = False
        self.phys_hps_tripped = False
        self.limit_trip_count = 0
        self.ignition_retry_count = 0
        self.force_pressure_snap = False
        
        self.sim_active = [False] * 16
        self.fault_active = [False] * 55

        # Refrigerant
        self.current_refrigerant = "R410A"
        self.id_is_txv = True
        
        # Ambient / Environment defaults
        self.set_od_temp = 90.0
        self.set_id_temp = 75.0
        self.set_rh = 50.0

        self.sim_od_low_press = 145.0
        self.sim_od_high_press = 145.0
        self.sim_od_liquid_press = 145.0
        self.sim_od_suction_temp = 90.0
        self.sim_od_liquid_temp = 90.0
        self.sim_od_discharge = 90.0
        self.sim_od_ambient = 90.0

        self.sim_id_ambient = 75.0
        self.sim_id_return_temp = 75.0
        self.sim_id_supply_temp = 75.0
        self.sim_id_rh = 50.0
        
        # Scenario / Login
        self.active_scenario = 0
        self.student_score = 100
        self.latest_diagnosis = "None"
        self.work_history_log = ""
        self.authRole = "none"
        self.simulation_cursor = 0
        self.mobile_app_last_seen = 0.0
        self.mobile_app_timeout_seconds = 15.0

    def reset_all_faults_and_sims(self):
        self.sim_active = [False] * 16
        self.fault_active = [False] * 55
        self.force_defrost = False
        self.furnace_state = "FURNACE_IDLE"
        self.active_scenario = 0
        self.inducer_on = False
        self.igniter_on = False
        self.gas_valve_on = False
        self.heat_blower_on = False

state = AppState()
users_db = load_users_db()

RUNTIME_EXCLUDED = {
    "edge_connected",
    "edge_last_seen",
    "edge_timeout_seconds",
    "ws_last_broadcast",
    "trainer_type",
    "selected_edge_id",
    "edges",
    "authRole",
    "simulation_cursor",
    "mobile_app_last_seen",
    "mobile_app_timeout_seconds",
}


def default_runtime() -> Dict[str, Any]:
    base = AppState()
    return {k: v for k, v in base.__dict__.items() if k not in RUNTIME_EXCLUDED}


def ensure_edge_runtime(edge: Dict[str, Any]) -> Dict[str, Any]:
    runtime = edge.get("runtime")
    if not isinstance(runtime, dict):
        runtime = default_runtime()
        edge["runtime"] = runtime
    return runtime


def apply_runtime_to_state(runtime: Dict[str, Any]) -> None:
    for k, v in runtime.items():
        if hasattr(state, k):
            setattr(state, k, v)


def save_state_to_runtime(runtime: Dict[str, Any]) -> None:
    for key in list(runtime.keys()):
        if hasattr(state, key):
            runtime[key] = getattr(state, key)


def persist_selected_runtime() -> None:
    selected = get_selected_edge()
    if not selected:
        return
    runtime = ensure_edge_runtime(selected)
    save_state_to_runtime(runtime)


def normalize_trainer_type(value: Optional[str]) -> str:
    incoming = (value or "").strip().lower().replace("-", "_").replace(" ", "_")
    if incoming in {"heat_pump", "heatpump", "hp"}:
        return "heat_pump"
    if incoming in {"ac_gas", "acgas", "furnace", "gas_furnace", "ac_furnace"}:
        return "ac_gas"
    return "ac_gas"


def get_edge_connected(last_seen: float) -> bool:
    return (time.time() - last_seen) <= state.edge_timeout_seconds


def get_selected_edge() -> Optional[Dict[str, Any]]:
    if not state.selected_edge_id:
        return None
    return state.edges.get(state.selected_edge_id)


def sync_selected_edge_into_state() -> None:
    selected = get_selected_edge()
    if not selected:
        state.edge_connected = False
        return

    runtime = ensure_edge_runtime(selected)
    apply_runtime_to_state(runtime)

    state.edge_last_seen = selected.get("last_seen", 0.0)
    state.edge_connected = get_edge_connected(state.edge_last_seen)
    state.trainer_type = selected.get("trainer_type", "ac_gas")

    state.state_w = bool(selected.get("w", state.state_w))
    state.state_y = bool(selected.get("y", state.state_y))
    state.state_g = bool(selected.get("g", state.state_g))
    state.phys_lps_tripped = bool(selected.get("phys_lps", state.phys_lps_tripped))
    state.phys_hps_tripped = bool(selected.get("phys_hps", state.phys_hps_tripped))

    runtime["state_w"] = state.state_w
    runtime["state_y"] = state.state_y
    runtime["state_g"] = state.state_g
    runtime["phys_lps_tripped"] = state.phys_lps_tripped
    runtime["phys_hps_tripped"] = state.phys_hps_tripped


def get_edges_payload() -> List[Dict[str, Any]]:
    now = time.time()
    rows: List[Dict[str, Any]] = []
    for edge_id, edge in state.edges.items():
        last_seen = float(edge.get("last_seen", 0.0))
        age_seconds = max(0.0, now - last_seen)
        connected = age_seconds <= state.edge_timeout_seconds
        rows.append(
            {
                "edge_id": edge_id,
                "label": edge.get("label", edge_id),
                "source_ip": edge.get("source_ip", ""),
                "trainer_type": edge.get("trainer_type", "ac_gas"),
                "connected": 1 if connected else 0,
                "last_seen": last_seen,
                "age_seconds": round(age_seconds, 1),
                "w_call": 1 if edge.get("w", False) else 0,
                "y_call": 1 if edge.get("y", False) else 0,
                "g_call": 1 if edge.get("g", False) else 0,
                "wifi_rssi": edge.get("wifi_rssi"),
                "ram": edge.get("ram"),
                "uptime": edge.get("uptime"),
                "temp": edge.get("temp"),
                "identity_reboot_required": 1 if edge.get("identity_reboot_required") else 0,
            }
        )

    # Stable ordering prevents UI cards from jumping around between refreshes.
    # Do not sort by transient status (online/offline), only by deterministic identity.
    rows.sort(key=lambda x: ((x.get("label") or "").lower(), x["edge_id"].lower()))
    return rows


def maybe_select_edge(edge_id: Optional[str]) -> None:
    if edge_id is None:
        return
    candidate = edge_id.strip()
    if not candidate:
        return
    if candidate in state.edges:
        if state.selected_edge_id and state.selected_edge_id != candidate:
            persist_selected_runtime()
        state.selected_edge_id = candidate
        sync_selected_edge_into_state()

# ==========================================
# BACKGROUND SIMULATION LOOP
# ==========================================

def add_noise(base: float, variance: float) -> float:
    r = random.random() # 0.0 to 1.0
    return base + ((r * (variance * 2.0)) - variance)

def get_refrigerant_multiplier(refrigerant: str) -> float:
    if refrigerant == "R22":
        return 0.60
    if refrigerant == "R32":
        return 1.04
    if refrigerant == "R454B":
        return 0.98
    if refrigerant == "R134a":
        return 0.40
    if refrigerant == "R404A":
        return 0.75
    if refrigerant == "R407C":
        return 0.65
    return 1.0

def get_pressure_switch_thresholds(refrigerant: str):
    # Default R410A thresholds
    lps_trip, lps_reset = 40.0, 80.0
    hps_trip, hps_reset = 610.0, 475.0

    if refrigerant == "R454B":
        lps_trip, lps_reset, hps_trip, hps_reset = 35.0, 70.0, 575.0, 450.0
    elif refrigerant == "R32":
        lps_trip, lps_reset, hps_trip, hps_reset = 45.0, 85.0, 640.0, 500.0
    elif refrigerant == "R22":
        lps_trip, lps_reset, hps_trip, hps_reset = 25.0, 60.0, 400.0, 300.0
    elif refrigerant == "R407C":
        lps_trip, lps_reset, hps_trip, hps_reset = 25.0, 55.0, 420.0, 320.0
    elif refrigerant == "R134a":
        lps_trip, lps_reset, hps_trip, hps_reset = 10.0, 25.0, 300.0, 200.0
    elif refrigerant == "R404A":
        lps_trip, lps_reset, hps_trip, hps_reset = 15.0, 35.0, 450.0, 350.0

    return lps_trip, lps_reset, hps_trip, hps_reset

def is_edge_ready() -> bool:
    if not EDGE_REQUIRED:
        return True
    return state.edge_connected and ((time.time() - state.edge_last_seen) <= state.edge_timeout_seconds)

async def simulation_loop():
    """ 
    Replaces the loop() function inside the ESP32. 
    Runs continuously in the background parsing physics and timers.
    """
    while True:
        now = time.time()
        sim_edge: Optional[Dict[str, Any]] = None
        if state.edges:
            edge_ids = sorted(state.edges.keys())
            if state.simulation_cursor >= len(edge_ids):
                state.simulation_cursor = 0
            sim_edge_id = edge_ids[state.simulation_cursor]
            state.simulation_cursor = (state.simulation_cursor + 1) % len(edge_ids)
            sim_edge = state.edges.get(sim_edge_id)
            if sim_edge is not None:
                runtime = ensure_edge_runtime(sim_edge)
                apply_runtime_to_state(runtime)
                state.edge_last_seen = sim_edge.get("last_seen", 0.0)
                state.edge_connected = get_edge_connected(state.edge_last_seen)
                state.trainer_type = sim_edge.get("trainer_type", "ac_gas")
                state.state_w = bool(sim_edge.get("w", state.state_w))
                state.state_y = bool(sim_edge.get("y", state.state_y))
                state.state_g = bool(sim_edge.get("g", state.state_g))
                state.state_o = bool(sim_edge.get("o", sim_edge.get("o_call", state.state_o)))
                state.is_b_type = bool(sim_edge.get("is_b_type", state.is_b_type))
                state.phys_lps_tripped = bool(sim_edge.get("phys_lps", state.phys_lps_tripped))
                state.phys_hps_tripped = bool(sim_edge.get("phys_hps", state.phys_hps_tripped))
        else:
            state.edge_connected = (now - state.edge_last_seen) <= state.edge_timeout_seconds

        # ==================================
        # 1. READ FAULT STATES
        # ==================================
        id_fan_fail = state.fault_active[24] or state.sim_active[1]
        od_fan_fail = state.fault_active[6] or state.sim_active[3]

        fault_non_condensables = state.fault_active[40]
        fault_stuck_id_txv     = state.fault_active[41]
        fault_stuck_od_txv     = state.fault_active[48]
        fault_clogged_txv      = state.fault_active[42]
        fault_clogged_piston   = state.fault_active[43]
        fault_comp_bypass      = state.fault_active[44]
        fault_inefficient_comp = state.fault_active[45]
        fault_low_id_cfm       = state.fault_active[46]
        fault_high_id_cfm      = state.fault_active[47]
        fault_rv_bypass        = state.fault_active[49]

        target_supply = state.set_id_temp 
        target_low = state.sim_od_low_press
        target_high = state.sim_od_high_press
        target_sh = 12.0

        rh_variance = (state.set_rh - 50.0) / 10.0
        latent_heat_penalty = (rh_variance * 1.5) if rh_variance > 0 else (rh_variance * 1.0)
        latent_pressure_penalty = (rh_variance * 3.0) if rh_variance > 0 else (rh_variance * 1.5)

        line_friction_delta = 0.0
        ref_mult = get_refrigerant_multiplier(state.current_refrigerant)
        target_eq_press = (145.0 + ((state.set_od_temp - 70.0) * 1.5)) * ref_mult

        # Pressure switch model (mirrors firmware behavior)
        lps_trip, lps_reset, hps_trip, hps_reset = get_pressure_switch_thresholds(state.current_refrigerant)
        if state.sim_od_low_press <= lps_trip:
            state.phys_lps_tripped = True
        elif state.sim_od_low_press >= lps_reset:
            state.phys_lps_tripped = False

        if state.sim_od_high_press >= hps_trip:
            state.phys_hps_tripped = True
        elif state.sim_od_high_press <= hps_reset:
            state.phys_hps_tripped = False

        lps_open = state.phys_lps_tripped or state.fault_active[7] or state.fault_active[26]
        hps_open = state.phys_hps_tripped or state.fault_active[8] or state.fault_active[27]
        y_broken = state.fault_active[2] or state.sim_active[14]
        grounded_w = state.fault_active[52]
        shorted_w_to_r = state.fault_active[53]
        shorted_y_to_r = state.fault_active[51]
        effective_w_call = state.state_w or grounded_w or shorted_w_to_r
        effective_y_call = state.state_y or shorted_y_to_r

        y_circuit_intact = effective_y_call and (not lps_open) and (not hps_open) and (not y_broken)
        contactor_pulled = y_circuit_intact or state.fault_active[30] or state.fault_active[54]
        is_compressor = contactor_pulled and (not state.fault_active[31]) and (not state.sim_active[15])

        # Hardware states derived from logic.
        # O/B reversing-valve semantics:
        # - O mode (is_b_type=0): O energized => cooling, O de-energized => heating
        # - B mode (is_b_type=1): B energized => heating, B de-energized => cooling
        if state.trainer_type == "heat_pump":
            rv_heating = state.state_o if state.is_b_type else (not state.state_o)
            # W call acts as aux/emergency heat overlay.
            phys_heating = (is_compressor and rv_heating) or effective_w_call
            phys_cooling = is_compressor and (not rv_heating)
        else: # ac_gas furnace
            phys_heating = effective_w_call
            phys_cooling = is_compressor

        # Override heating state with firmware-native simulation if available
        if sim_edge and sim_edge.get("telemetry", {}).get("sim_flame_active"):
            phys_heating = True
            phys_cooling = is_compressor and (not rv_heating)
            # W call acts as aux/emergency heat overlay.
            phys_heating = (is_compressor and rv_heating) or effective_w_call
        else:
            phys_heating = effective_w_call
            phys_cooling = is_compressor

        # ==================================
        # 2. REFRIGERANT PHYSICS LOGIC
        # ==================================
        if not is_compressor:
            target_low = target_eq_press
            target_high = target_eq_press
            bleed_rate = 0.005 if state.id_is_txv else 0.025
            
            state.sim_od_high_press += (target_eq_press - state.sim_od_high_press) * bleed_rate
            state.sim_od_low_press += (target_eq_press - state.sim_od_low_press) * bleed_rate
            
            state.sim_od_suction_temp += (state.set_od_temp - state.sim_od_suction_temp) * 0.05
            state.sim_od_liquid_temp += (state.set_od_temp - state.sim_od_liquid_temp) * 0.05
            state.sim_od_discharge += (state.set_od_temp - state.sim_od_discharge) * 0.05
            target_supply = state.set_id_temp + (65.0 if phys_heating else 0.0)

        elif phys_cooling:
            target_high = ((state.set_od_temp * 3.5) + 50.0) * ref_mult 
            txv_shift = 1.0 if state.id_is_txv else 0.0
            target_low = ((state.set_id_temp * 2.0) - 30.0 + (latent_pressure_penalty * txv_shift)) * ref_mult   
            target_sh = 12.0 if state.id_is_txv else ((state.set_id_temp - state.set_od_temp + 30.0) + (latent_heat_penalty * 2.0))

            line_friction_delta = 8.0 if od_fan_fail else 18.0 

            if fault_non_condensables: 
                target_high += 130.0 * ref_mult
                target_low += 5.0 * ref_mult
                line_friction_delta += 10.0
            if fault_stuck_id_txv: 
                target_low += 25.0 * ref_mult
                target_high -= 30.0 * ref_mult
                target_sh = 0.5
            if fault_clogged_txv or fault_clogged_piston: 
                target_low -= 35.0 * ref_mult
                target_high -= 15.0 * ref_mult
                target_sh = 35.0
            if fault_comp_bypass: 
                target_low += 40.0 * ref_mult
                target_high -= 75.0 * ref_mult
            if fault_inefficient_comp: 
                target_low += 25.0 * ref_mult
                target_high -= 45.0 * ref_mult
            if fault_rv_bypass:
                target_low += 50.0 * ref_mult
                target_high -= 80.0 * ref_mult
                target_sh += 15.0
            if fault_low_id_cfm: 
                target_low -= 20.0 * ref_mult
                target_sh = 2.0
                target_supply -= 12.0
                line_friction_delta -= 6.0
            if fault_high_id_cfm: 
                target_low += 15.0 * ref_mult
                target_sh += 15.0
                target_supply += 8.0
                line_friction_delta += 5.0

            low_abs = max(state.sim_od_low_press + 14.7, 1.0)
            comp_ratio = (state.sim_od_high_press + 14.7) / low_abs
            vol_eff_penalty = max((comp_ratio - 2.5) * 4.0, 0.0)

            target_low += vol_eff_penalty * ref_mult 
            
            if od_fan_fail: target_high = 600.0 * ref_mult 
            if id_fan_fail: 
                target_low = 20.0 * ref_mult
                line_friction_delta = 2.0

            estimated_low_sat = (target_low / ref_mult) * 0.3 + 10.0 
            state.sim_od_suction_temp = add_noise(estimated_low_sat + target_sh, 0.4)  
            state.sim_od_liquid_temp = add_noise(state.set_od_temp + 10.0, 0.4)  
            state.sim_od_discharge = add_noise(state.sim_od_suction_temp + (comp_ratio * 20.0) + 45.0 + vol_eff_penalty, 2.0)
            target_supply = (state.set_id_temp - 20.0) + latent_heat_penalty

            if fault_comp_bypass: state.sim_od_suction_temp += 35.0 
            if fault_rv_bypass: state.sim_od_suction_temp += 45.0
            if fault_non_condensables: state.sim_od_liquid_temp -= 12.0 
            if fault_clogged_txv or fault_clogged_piston: state.sim_od_liquid_temp -= 15.0 

            if od_fan_fail: state.sim_od_discharge = add_noise(220.0, 5.0)
            if id_fan_fail: state.sim_od_suction_temp = add_noise(25.0, 0.5) 

            if phys_heating: target_supply += 65.0 # Ensure heat is simulated if both run simultaneously

        elif phys_heating:
            target_low = ((state.set_od_temp * 1.8) + 25.0) * ref_mult
            base_head = (state.set_id_temp * 3.0) + (state.set_od_temp * 1.5) + 50.0
            extreme_ambient_penalty = 0.0
            if state.set_od_temp > 65.0:
                excess = state.set_od_temp - 65.0
                extreme_ambient_penalty = (excess * excess * 1.2)
            target_high = (base_head + extreme_ambient_penalty) * ref_mult
            target_sh = 10.0 if state.id_is_txv else 15.0

            line_friction_delta = 7.0 if id_fan_fail else 24.0

            if fault_non_condensables:
                target_high += 130.0 * ref_mult
                target_low += 5.0 * ref_mult
                line_friction_delta += 12.0
            if fault_stuck_od_txv or fault_clogged_txv or fault_clogged_piston:
                target_low -= 35.0 * ref_mult
                target_high -= 15.0 * ref_mult
                target_sh = 35.0
            if fault_comp_bypass:
                target_low += 40.0 * ref_mult
                target_high -= 75.0 * ref_mult
            if fault_inefficient_comp:
                target_low += 25.0 * ref_mult
                target_high -= 45.0 * ref_mult
            if fault_rv_bypass:
                target_low += 50.0 * ref_mult
                target_high -= 80.0 * ref_mult
                target_sh += 15.0
            if fault_low_id_cfm:
                target_high += 55.0 * ref_mult
                target_supply += 18.0
                line_friction_delta += 10.0
            if fault_high_id_cfm:
                target_high -= 25.0 * ref_mult
                target_supply -= 9.0
                line_friction_delta -= 6.0

            low_abs = max(state.sim_od_low_press + 14.7, 1.0)
            comp_ratio = (state.sim_od_high_press + 14.7) / low_abs
            vol_eff_penalty = max((comp_ratio - 2.5) * 4.0, 0.0)

            target_low += vol_eff_penalty * ref_mult

            if od_fan_fail:
                target_low = 20.0 * ref_mult
            if id_fan_fail:
                target_high = 650.0 * ref_mult

            estimated_low_sat = (target_low / ref_mult) * 0.3 + 10.0
            state.sim_od_suction_temp = add_noise(estimated_low_sat + target_sh, 0.4)
            state.sim_od_liquid_temp = add_noise(state.set_id_temp + 12.0 + (extreme_ambient_penalty * 0.05), 0.4)
            state.sim_od_discharge = add_noise(
                state.sim_od_suction_temp + (comp_ratio * 24.0) + 55.0 + extreme_ambient_penalty + vol_eff_penalty,
                2.0,
            )
            target_supply = (state.set_id_temp + 27.0) + (extreme_ambient_penalty * 0.1)

            if fault_comp_bypass:
                state.sim_od_suction_temp += 35.0
            if fault_rv_bypass:
                state.sim_od_suction_temp += 45.0
            if fault_non_condensables:
                state.sim_od_liquid_temp -= 12.0
            if fault_clogged_txv or fault_clogged_piston:
                state.sim_od_liquid_temp -= 15.0

            if id_fan_fail:
                state.sim_od_discharge = add_noise(250.0, 5.0)
            if od_fan_fail:
                state.sim_od_suction_temp = add_noise(5.0, 0.5)
        
        # Furnace-specific temperature simulation
        if state.trainer_type == "ac_gas" and phys_heating:
            target_supply = 130.0


        if state.force_pressure_snap and is_compressor:
            state.sim_od_low_press = target_low
            state.sim_od_high_press = target_high
            state.sim_od_liquid_press = target_high - (line_friction_delta * 1.8 * ref_mult)
            state.force_pressure_snap = False
        elif is_compressor:
            state.sim_od_low_press += (target_low - state.sim_od_low_press) * 0.045
            state.sim_od_high_press += (target_high - state.sim_od_high_press) * 0.065
            true_liquid_target = target_high - (line_friction_delta * 1.8 * ref_mult)
            state.sim_od_liquid_press += (true_liquid_target - state.sim_od_liquid_press) * 0.020
        else:
            state.sim_od_liquid_press += (target_high - state.sim_od_liquid_press) * 0.012


        if id_fan_fail: 
            target_supply = 160.0 if phys_heating else state.set_id_temp
            
        state.sim_id_supply_temp += (target_supply - state.sim_id_supply_temp) * 0.05
        state.sim_od_ambient = add_noise(state.set_od_temp, 0.06)
        state.sim_id_return_temp = add_noise(state.set_id_temp, 0.06)
        state.sim_id_ambient = add_noise(state.set_id_temp, 0.06)
        state.sim_id_rh = add_noise(state.set_rh, 0.18)

        # ==================================
        # 3. ELECTRICAL TELEMETRY MATH
        # ==================================
        if is_compressor and not state.last_comp_state:
            state.comp_start_time = now
        state.last_comp_state = is_compressor

        if is_compressor:
            if state.fault_active[50] or (now - state.comp_start_time < 0.4): # Locked rotor or startup
                state.sim_comp_amps = 143.0
            else:
                amps = 10.0 + ((state.sim_od_high_press / ref_mult) * 0.035)
                if fault_comp_bypass: amps -= 6.5
                if fault_inefficient_comp: amps -= 4.0
                state.sim_comp_amps = add_noise(amps, 0.2)
        else:
            state.sim_comp_amps = 0.0

        if is_compressor and not od_fan_fail:
            state.sim_od_fan_amps = add_noise(1.8, 0.05)
        else:
            state.sim_od_fan_amps = 0.0

        id_fan_on = (state.state_g or effective_y_call or state.heat_blower_on or state.fault_active[25]) and not id_fan_fail
        if id_fan_on:
            base_id_amps = 4.5
            if fault_low_id_cfm: base_id_amps = 3.6
            if fault_high_id_cfm: base_id_amps = 5.3
            state.sim_id_fan_amps = add_noise(base_id_amps, 0.08)
        else:
            state.sim_id_fan_amps = 0.0


        # ==================================
        # 4. HVAC FURNACE STATE MACHINE
        # ==================================
        if state.furnace_state == "FURNACE_IDLE":
            if state.blower_off_delay > 0 and now >= state.blower_off_delay:
                state.inducer_on = False
                state.igniter_on = False
                state.gas_valve_on = False
                state.heat_blower_on = False
                state.blower_off_delay = 0
            if effective_w_call:
                state.furnace_state = "FURNACE_PRE_PURGE"
                state.furnace_timer = now
                state.ignition_retry_count = 0
                state.inducer_on = True

        elif state.furnace_state == "FURNACE_PRE_PURGE":
            if not effective_w_call:
                state.furnace_state = "FURNACE_POST_PURGE"
                state.furnace_timer = now
            elif now - state.furnace_timer >= 15.0:
                pressure_switch_closed = not state.fault_active[16] and not state.fault_active[23] and not state.fault_active[17] # Matches f16, f23, f17
                limit_ok = not state.fault_active[22] and not state.fault_active[33] and not state.fault_active[34] # Matches f22, f33, f34
                
                if pressure_switch_closed and limit_ok:
                    state.furnace_state = "FURNACE_IGNITER_WARMUP"
                    state.furnace_timer = now
                    state.igniter_on = True
                else:
                    state.furnace_state = "FURNACE_LOCKOUT"
                    # Inducer remains on for safety as per firmware logic

        elif state.furnace_state == "FURNACE_IGNITER_WARMUP":
            if not effective_w_call:
                state.furnace_state = "FURNACE_POST_PURGE"
                state.furnace_timer = now
                state.igniter_on = False
            elif now - state.furnace_timer >= 10.0:
                state.furnace_state = "FURNACE_TRIAL_FOR_IGNITION"
                state.furnace_timer = now
                state.gas_valve_on = True

        elif state.furnace_state == "FURNACE_TRIAL_FOR_IGNITION":
            if not effective_w_call:
                state.furnace_state = "FURNACE_POST_PURGE"
                state.furnace_timer = now
                state.igniter_on = False
                state.gas_valve_on = False
            elif now - state.furnace_timer >= 4.0:
                flame_sensed = not state.fault_active[32] and not state.fault_active[18] and not state.fault_active[21] # Matches f32, f18, f21
                if flame_sensed:
                    state.furnace_state = "FURNACE_HEATING"
                    state.igniter_on = False
                    state.blower_on_delay = now + 30.0
                else:
                    state.ignition_retry_count += 1
                    state.igniter_on = False # Turn off igniter on failure
                    state.gas_valve_on = False
                    state.furnace_state = "FURNACE_PRE_PURGE" if state.ignition_retry_count < 3 else "FURNACE_LOCKOUT" # Retry or lockout
                    state.furnace_timer = now

        elif state.furnace_state == "FURNACE_HEATING":
            if not effective_w_call:
                state.gas_valve_on = False
                state.furnace_state = "FURNACE_POST_PURGE"
                state.furnace_timer = now
            elif state.sim_id_supply_temp > 150.0: # High limit trip
                state.furnace_state = "FURNACE_LOCKOUT"
                state.gas_valve_on = False
                state.igniter_on = False
                state.inducer_on = True
                state.heat_blower_on = True
                state.limit_trip_count += 1
                # Keeps blower running to cool down
            elif state.blower_on_delay > 0 and now >= state.blower_on_delay:
                state.heat_blower_on = True
                state.blower_on_delay = 0

        elif state.furnace_state == "FURNACE_POST_PURGE":
            if now - state.furnace_timer >= 15.0:
                state.inducer_on = False
                state.furnace_state = "FURNACE_IDLE"
                state.blower_off_delay = now + 90.0

        elif state.furnace_state == "FURNACE_LOCKOUT":
            if not effective_w_call:
                # If call for heat is removed, go to post-purge to cool down and then idle
                state.furnace_state = "FURNACE_POST_PURGE"
                state.furnace_timer = now
            elif state.sim_id_supply_temp < 110.0 and state.limit_trip_count > 0:
                # Auto-reset from a high-limit trip once the furnace has cooled down
                state.furnace_state = "FURNACE_PRE_PURGE"
                state.furnace_timer = now
                # Relays are handled by the target state

        if sim_edge is not None:
            runtime = ensure_edge_runtime(sim_edge)
            runtime["state_w"] = state.state_w
            runtime["state_y"] = state.state_y
            runtime["state_g"] = state.state_g
            runtime["phys_lps_tripped"] = state.phys_lps_tripped
            runtime["phys_hps_tripped"] = state.phys_hps_tripped
            sim_edge["w"] = state.state_w
            sim_edge["y"] = state.state_y
            sim_edge["g"] = state.state_g
            sim_edge["phys_lps"] = state.phys_lps_tripped
            sim_edge["phys_hps"] = state.phys_hps_tripped
            save_state_to_runtime(runtime)

        sync_selected_edge_into_state()

        # Broadcast selected trainer telemetry over WebSockets once per loop window.
        if active_websockets and (now - state.ws_last_broadcast) >= WS_BROADCAST_INTERVAL_SECONDS:
            status_data = await get_status()
            disconnected = []
            for ws in active_websockets:
                try:
                    await ws.send_json(status_data)
                except Exception:
                    disconnected.append(ws)
            for ws in disconnected:
                if ws in active_websockets:
                    active_websockets.remove(ws)
            state.ws_last_broadcast = now

        await asyncio.sleep(SIMULATION_TICK_SECONDS)

@app.on_event("startup")
async def startup_event():
    global mqtt_task
    state.edges = load_edges_db()
    if state.edges:
        state.selected_edge_id = sorted(state.edges.keys(), key=lambda k: k.lower())[0]
        sync_selected_edge_into_state()
    with suppress(Exception):
        await asyncio.to_thread(start_mdns_advertisement)
    asyncio.create_task(simulation_loop())
    mqtt_task = asyncio.create_task(mqtt_command_listener())


@app.on_event("shutdown")
async def shutdown_event():
    global mqtt_task, mqtt_client
    if mqtt_task is not None:
        mqtt_task.cancel()
        with suppress(asyncio.CancelledError):
            await mqtt_task
    mqtt_task = None
    mqtt_client = None
    stop_mdns_advertisement()

# -------------------------------------------------
# MQTT command listener – forwards incoming commands to WebSocket clients or updates state as needed
# -------------------------------------------------
async def mqtt_command_listener():
    global mqtt_client
    while True:
        try:
            async with MQTTClient(MQTT_HOST) as client:
                mqtt_client = client
                await client.subscribe("trainer/+/command")
                async for message in client.messages:
                    topic = str(message.topic)
                    payload = message.payload.decode()
                    try:
                        data = json.loads(payload)
                    except json.JSONDecodeError:
                        print(f"[MQTT] Invalid JSON on {topic}: {payload}")
                        continue

                    # Expected topic format: trainer/{trainer_id}/command
                    parts = topic.split('/')
                    if len(parts) != 3:
                        continue
                    _, trainer_id, _ = parts

                    # For now, simply broadcast the command to any connected WebSocket clients.
                    if active_websockets:
                        for ws in active_websockets:
                            try:
                                await ws.send_json({"trainer_id": trainer_id, "command": data})
                            except Exception:
                                pass
        except MqttError as e:
            mqtt_client = None
            print(f"[MQTT] Connection error: {e}")
            await asyncio.sleep(2)

# -------------------------------------------------
# REST endpoint to send commands to a specific trainer via MQTT
# -------------------------------------------------
@app.post("/api/trainer/{trainer_id}/command")
async def send_trainer_command(trainer_id: str, command: Dict[str, Any]):
    """Publish a command to a trainer over MQTT. The command payload should be a JSON object, e.g. {"action": "reset"} or {"action": "grade", "score": 95}."""
    if mqtt_client is None:
        raise HTTPException(status_code=500, detail="MQTT client not initialized")
    topic = f"trainer/{trainer_id}/command"
    payload = json.dumps(command)
    try:
        await mqtt_client.publish(topic, payload, qos=1)
    except MqttError as e:
        raise HTTPException(status_code=500, detail=f"Failed to publish MQTT message: {e}")
    return {"message": "command sent", "trainer_id": trainer_id}

# ==========================================
# API ROUTES (Replaces server.on(...) in main.cpp)
# ==========================================

@app.get("/api/status")
async def get_status():
    """ Returns the complete JSON state of the system for the frontend """
    
    # Simulate simple noise for realistic gauges as done in C++
    low_noise = ((random.random() * 0.8) - 0.4)
    high_noise = ((random.random() * 1.2) - 0.6)
    liquid_noise = ((random.random() * 1.0) - 0.5)

    # Map Python FurnaceState string to C++ enum integer to keep frontend happy
    furnace_map = {
        "FURNACE_IDLE": 0, "FURNACE_PRE_PURGE": 1, "FURNACE_IGNITER_WARMUP": 2,
        "FURNACE_TRIAL_FOR_IGNITION": 3, "FURNACE_HEATING": 4, "FURNACE_POST_PURGE": 5, "FURNACE_LOCKOUT": 6
    }
    
    sync_selected_edge_into_state()
    edge_ready = is_edge_ready()
    selected_edge = get_selected_edge()

    uptime_sec = int(time.time())
    now = time.time()
    mobile_app_connected = (now - state.mobile_app_last_seen) <= state.mobile_app_timeout_seconds

    uptime_fmt = f"{(uptime_sec // 3600) % 100:02d}:{(uptime_sec % 3600) // 60:02d}:{uptime_sec % 60:02d}"
    hs_amps = ((1.2 if state.inducer_on else 0.0) + (3.5 if state.igniter_on else 0.0) + (0.5 if state.gas_valve_on else 0.0)) if state.heat_blower_on else 0.0

    payload = {
        "edge_required": 1 if EDGE_REQUIRED else 0,
        "edge_connected": 1 if edge_ready else 0,
        "selected_edge_id": state.selected_edge_id,
        "mobile_app_connected": 1 if mobile_app_connected else 0,
        "selected_edge_label": selected_edge.get("label") if selected_edge else None,
        "trainer_type": state.trainer_type,
        "w_call": state.state_w or state.fault_active[52] or state.fault_active[53],
        "y_call": state.state_y or state.fault_active[51],
        "g_call": state.state_g,
        "o_call": state.state_o,
        "is_b_type": 1 if state.is_b_type else 0,
        "force_defrost": 1 if state.force_defrost else 0,
        
        "diagnosis": state.latest_diagnosis,
        "student_score": state.student_score,
        "work_history": state.work_history_log,
        
        "refrigerant": state.current_refrigerant,
        "id_is_txv": 1 if state.id_is_txv else 0,
        "indoor_metering": "TXV" if state.id_is_txv else "Piston",
        
        "set_od": round(state.set_od_temp, 1),
        "set_id": round(state.set_id_temp, 1),
        "set_rh": round(state.set_rh, 1),
        
        "phys_lps": 1 if state.phys_lps_tripped else 0,
        "phys_hps": 1 if state.phys_hps_tripped else 0,
        # Safeties map
        "lps_open": 1 if (state.phys_lps_tripped or state.fault_active[7] or state.fault_active[26]) else 0,
        "hps_open": 1 if (state.phys_hps_tripped or state.fault_active[8] or state.fault_active[27]) else 0,
        
        "od_low_press": round(state.sim_od_low_press + low_noise, 1) if edge_ready else None,
        "od_high_press": round(state.sim_od_high_press + high_noise, 1) if edge_ready else None,
        "od_liquid_press": round(state.sim_od_liquid_press + liquid_noise, 1) if edge_ready else None,
        "od_suction_temp": round(state.sim_od_suction_temp, 1) if edge_ready else None,
        "od_liquid_temp": round(state.sim_od_liquid_temp, 1) if edge_ready else None,
        "od_ambient": round(state.sim_od_ambient, 1) if edge_ready else None,
        "od_discharge_temp": round(state.sim_od_discharge, 1) if edge_ready else None,
        
        "id_return_temp": round(state.sim_id_return_temp, 1) if edge_ready else None,
        "id_supply_temp": round(state.sim_id_supply_temp, 1) if edge_ready else None,
        "id_ambient": round(state.sim_id_ambient, 1) if edge_ready else None,
        "id_rh": round(state.sim_id_rh, 1) if edge_ready else None,
        "id_suction_temp": round(state.sim_od_suction_temp + 1.2, 1) if edge_ready else None,
        "id_liquid_temp": round(state.sim_od_liquid_temp - 1.2, 1) if edge_ready else None,
        
        "comp_amps": round(state.sim_comp_amps, 1) if edge_ready else None,
        "od_fan_amps": round(state.sim_od_fan_amps, 1) if edge_ready else None,
        "id_fan_amps": round(state.sim_id_fan_amps, 1) if edge_ready else None,
        "hs_amps": round(hs_amps, 1) if edge_ready else None,

        "wifi_rssi": selected_edge.get("wifi_rssi", -55) if edge_ready and selected_edge else (0 if EDGE_REQUIRED else -55),
        "ram": selected_edge.get("ram", 245760) if selected_edge else 245760,
        "uptime": selected_edge.get("uptime", uptime_fmt) if selected_edge else uptime_fmt,
        "clients": len(active_websockets),
        "temp": round(98.0 + random.uniform(-0.4, 0.4), 1),
        
        "active_scenario": state.active_scenario,
        "reset_counter": 0,
        "ble_login_status": "None",
        
        "furnace_state": furnace_map.get(state.furnace_state, 0),
        "inducer_on": 1 if state.inducer_on else 0,
        "igniter_on": 1 if state.igniter_on else 0,
        "gas_valve_on": 1 if state.gas_valve_on else 0,
        "heat_blower_on": 1 if state.heat_blower_on else 0
    }

    # Additional relay aliases used by instructor fault-control scenarios.
    payload["relay_failed_gas_valve"] = 1 if state.fault_active[21] else 0
    payload["relay_faulty_high_temp_limit"] = 1 if state.fault_active[22] else 0
    payload["relay_grounded_w_wire"] = 1 if state.fault_active[52] else 0
    payload["relay_shorted_y_to_r"] = 1 if state.fault_active[51] else 0
    payload["relay_shorted_w_to_r"] = 1 if state.fault_active[53] else 0
    payload["relay_draft_inducer_board_fault"] = 1 if state.fault_active[17] else 0
    payload["relay_shorted_ignition_board"] = 1 if state.fault_active[18] else 0
    payload["relay_faulty_a2l_sensor"] = 1 if state.fault_active[28] else 0
    payload["relay_faulty_a2l_board"] = 1 if state.fault_active[29] else 0
    payload["relay_open_contactor_coil"] = 1 if state.fault_active[31] else 0
    payload["relay_shorted_contactor_coil"] = 1 if state.fault_active[54] else 0

    # Merge pass-through telemetry from the selected edge (if present), so UI can use
    # firmware-native keys like fXX/sim_XX/heat-strip relay aliases.
    if selected_edge and isinstance(selected_edge.get("telemetry"), dict):
        payload.update(selected_edge["telemetry"])

    # Guarantee full fault/simulation key coverage for instructor UI rendering.
    for fault_idx in range(1, 55):
        key = f"f{fault_idx}"
        payload.setdefault(key, 1 if state.fault_active[fault_idx] else 0)

    for sim_idx in range(1, 16):
        key = f"sim_{sim_idx:02d}"
        payload.setdefault(key, 1 if state.sim_active[sim_idx] else 0)

    return payload


class ThermostatUpdate(BaseModel):
    w: Optional[bool] = None
    y: Optional[bool] = None
    g: Optional[bool] = None
    o: Optional[bool] = None
    is_b_type: Optional[bool] = None

@app.post("/api/thermostat")
async def update_thermostat(req: ThermostatUpdate, edge_id: Optional[str] = Query(default=None)):
    """ External input API to click the 'thermostat' (simulating relay inputs) """
    maybe_select_edge(edge_id)
    if req.w is not None: state.state_w = req.w
    if req.y is not None: state.state_y = req.y
    if req.g is not None: state.state_g = req.g
    if req.o is not None: state.state_o = req.o
    if req.is_b_type is not None: state.is_b_type = req.is_b_type
    persist_selected_runtime()
    return {"message": "OK"}


class AmbientUpdate(BaseModel):
    od: Optional[float] = None
    id: Optional[float] = None
    rh: Optional[float] = None

@app.post("/api/ambient")
async def update_ambient(
    req: Optional[AmbientUpdate] = None,
    od: Optional[float] = Query(default=None),
    id: Optional[float] = Query(default=None),
    rh: Optional[float] = Query(default=None),
    edge_id: Optional[str] = Query(default=None),
):
    maybe_select_edge(edge_id)
    # Accept both query-string updates (current frontend behavior) and JSON body updates.
    if req is not None:
        if req.od is not None and od is None:
            od = req.od
        if req.id is not None and id is None:
            id = req.id
        if req.rh is not None and rh is None:
            rh = req.rh

    # Only update fields that were provided; never reset others implicitly.
    if od is not None:
        state.set_od_temp = od
    if id is not None:
        state.set_id_temp = id
    if rh is not None:
        state.set_rh = rh

    persist_selected_runtime()

    return {"message": "OK"}


@app.post("/api/reset")
async def system_reset(edge_id: Optional[str] = Query(default=None)):
    maybe_select_edge(edge_id)
    state.reset_all_faults_and_sims()
    persist_selected_runtime()
    return {"message": "OK"}


class ToggleRequest(BaseModel):
    id: str
    state: int

@app.post("/api/toggle")
async def toggle_state(
    req: Optional[ToggleRequest] = None,
    id: Optional[str] = Query(default=None),
    state_value: Optional[int] = Query(default=None, alias="state"),
    edge_id: Optional[str] = Query(default=None),
):
    """ Used by the instructor portal to turn on faults, sims, and physical relays. """
    maybe_select_edge(edge_id)

    if req is not None:
        if id is None:
            id = req.id
        if state_value is None:
            state_value = req.state

    if id is None or state_value is None:
        raise HTTPException(status_code=400, detail="id and state are required")

    state_bool = True if state_value == 1 else False
    
    if id == "reset_score":
        state.student_score = 100
        state.work_history_log = ""
        state.latest_diagnosis = "None"
    elif id == "force_defrost":
        state.force_defrost = state_bool
    elif id.startswith("sim_"):
        try:
            simNum = int(id[4:])
            if 1 <= simNum <= 15:
                state.sim_active[simNum] = state_bool
        except ValueError:
            pass
    elif id.startswith("f"):
        try:
            f = int(id[1:])
            if 0 <= f < len(state.fault_active):
                state.fault_active[f] = state_bool
        except ValueError:
            pass
    else:
        # Standard relay toggles (inducer, igniter, gas valve, blower, compressor) are handled here.
        # In a purely software Docker environment, we just update internal state tracking.
        if id == "relay_inducer": state.inducer_on = state_bool
        elif id == "relay_igniter": state.igniter_on = state_bool
        elif id == "relay_gas_valve": state.gas_valve_on = state_bool
        elif id == "relay_heat_blower": state.heat_blower_on = state_bool
        elif id == "relay_rollout_limit_1": state.fault_active[33] = state_bool
        elif id == "relay_rollout_limit_2": state.fault_active[34] = state_bool
        elif id == "relay_high_temp_limit": state.fault_active[22] = state_bool
        elif id == "relay_flame_sensor": state.fault_active[32] = state_bool
        elif id == "relay_condenser_fan": state.fault_active[6] = state_bool
        elif id == "relay_low_pressure_switch": state.fault_active[7] = state_bool
        elif id == "relay_high_pressure_switch": state.fault_active[8] = state_bool
        elif id == "relay_vacuum_pressure_switch": state.fault_active[16] = state_bool
        elif id == "relay_pressure_switch_closed": state.fault_active[19] = state_bool
        elif id == "relay_inducer_open": state.fault_active[15] = state_bool
        elif id == "relay_igniter_open": state.fault_active[18] = state_bool
        elif id == "relay_gas_valve_closed": state.fault_active[21] = state_bool
        elif id == "relay_gas_valve_open": state.fault_active[20] = state_bool
        elif id == "relay_draft_safeguard": state.fault_active[17] = state_bool
        elif id == "relay_blocked_flue": state.fault_active[23] = state_bool
        elif id == "relay_indoor_fan_off": state.fault_active[24] = state_bool
        elif id == "relay_indoor_fan_on": state.fault_active[25] = state_bool
        elif id == "relay_shorted_contactor": state.fault_active[30] = state_bool
        elif id == "relay_comp_limit_open": state.fault_active[31] = state_bool
        elif id == "relay_low_pressure_board_fault": state.fault_active[26] = state_bool
        elif id == "relay_high_pressure_board_fault": state.fault_active[27] = state_bool
        elif id == "relay_failed_gas_relay": state.fault_active[34] = state_bool
        elif id == "relay_a2l_sensor": state.fault_active[28] = state_bool
        elif id == "relay_a2l_board_fault": state.fault_active[29] = state_bool
        elif id == "relay_failed_gas_valve": state.fault_active[21] = state_bool
        elif id == "relay_faulty_high_temp_limit": state.fault_active[22] = state_bool
        elif id == "relay_grounded_w_wire": state.fault_active[52] = state_bool
        elif id == "relay_shorted_y_to_r": state.fault_active[51] = state_bool
        elif id == "relay_shorted_w_to_r": state.fault_active[53] = state_bool
        elif id == "relay_draft_inducer_board_fault": state.fault_active[17] = state_bool
        elif id == "relay_shorted_ignition_board": state.fault_active[18] = state_bool
        elif id == "relay_faulty_a2l_sensor": state.fault_active[28] = state_bool
        elif id == "relay_faulty_a2l_board": state.fault_active[29] = state_bool
        elif id == "relay_open_contactor_coil": state.fault_active[31] = state_bool
        elif id == "relay_shorted_contactor_coil": state.fault_active[54] = state_bool

    persist_selected_runtime()

    return {"message": "OK"}


def get_expected_diagnosis():
    if state.fault_active[46]: return "Low Indoor Airflow"
    if state.fault_active[47]: return "High Indoor Airflow"
    if state.fault_active[40]: return "Non-Condensables"
    if state.fault_active[41]: return "Stuck Indoor TXV"
    if state.fault_active[42]: return "Clogged TXV"
    if state.fault_active[43]: return "Clogged Piston"
    if state.fault_active[44]: return "Compressor Internal Bypass"
    if state.fault_active[45]: return "Inefficient Compressor"
    
    if state.fault_active[24] or state.sim_active[1] or state.sim_active[6]: return "Failed Indoor Blower"
    if state.fault_active[6] or state.sim_active[3]: return "Failed Condenser Fan"
    if state.sim_active[15] or state.fault_active[31] or state.fault_active[4]: return "Failed Compressor / Overload"
    if state.fault_active[15]: return "Failed Inducer Motor"
    if state.fault_active[18]: return "Failed Hot Surface Igniter"
    if state.fault_active[21]: return "Stuck Gas Valve (Closed)"
    if state.fault_active[20]: return "Stuck Gas Valve (Open)"
    if state.fault_active[32]: return "Dirty Flame Sensor"
    if state.fault_active[16]: return "Pressure Switch Stuck Open"
    if state.fault_active[19]: return "Pressure Switch Stuck Closed"
    if state.fault_active[22]: return "Open High Limit Switch"
    if state.fault_active[33]: return "Open Rollout Switch"

    if any(state.fault_active) or any(state.sim_active):
        return "Unknown Fault"
    
    return "Normal Operation"


class SubmitDiagnosis(BaseModel):
    diagnosis: str

@app.post("/api/submit")
async def submit_diagnosis(
    req: Optional[SubmitDiagnosis] = None,
    diagnosis: Optional[str] = Query(default=None),
    edge_id: Optional[str] = Query(default=None),
):
    if req is not None and diagnosis is None:
        diagnosis = req.diagnosis
    if diagnosis is None or len(diagnosis.strip()) == 0:
        raise HTTPException(status_code=400, detail="diagnosis is required")

    # --- Mark that we've heard from a mobile app ---
    state.mobile_app_last_seen = time.time()

    # --- FIX: Operate on the specific edge that submitted the diagnosis ---
    submitting_edge_id = (edge_id or "").strip()
    if not submitting_edge_id or submitting_edge_id not in state.edges:
        raise HTTPException(status_code=404, detail="Submitting edge not found")

    # Temporarily load the submitting edge's runtime state
    submitting_edge = state.edges[submitting_edge_id]
    runtime = ensure_edge_runtime(submitting_edge)
    apply_runtime_to_state(runtime)

    # Now, grade against the correct state
    expected = get_expected_diagnosis()
    state.latest_diagnosis = diagnosis # Update the temporary state
    
    if diagnosis.upper() == expected.upper() or "CORRECT" in diagnosis.upper():
        state.reset_all_faults_and_sims()
        state.work_history_log += f"[{time.strftime('%H:%M:%S')}] Submitted {diagnosis}: CORRECT\n"
        save_state_to_runtime(runtime)

        if submitting_edge_id and mqtt_client:
            try:
                await mqtt_client.publish(f"trainer/{submitting_edge_id}/command", json.dumps({"action": "reset"}), qos=1)
                print(f"Sent MQTT reset command to {submitting_edge_id} after correct diagnosis.")
            except Exception as e:
                print(f"Failed to send MQTT reset command to {submitting_edge_id}: {e}")
        
        if state.selected_edge_id != submitting_edge_id:
            sync_selected_edge_into_state()
        return Response(content="CORRECT", media_type="text/plain")
    else:
        state.work_history_log += f"[{time.strftime('%H:%M:%S')}] Submitted {diagnosis}: INCORRECT\n"
        state.student_score -= 10
        if state.student_score < 0: state.student_score = 0
        save_state_to_runtime(runtime)
        if state.selected_edge_id != submitting_edge_id:
            sync_selected_edge_into_state()
        return Response(content="INCORRECT", media_type="text/plain")


class RefrigerantUpdate(BaseModel):
    type: str

@app.post("/api/refrigerant")
async def update_refrigerant(
    req: Optional[RefrigerantUpdate] = None,
    type: Optional[str] = Query(default=None),
    edge_id: Optional[str] = Query(default=None),
):
    maybe_select_edge(edge_id)
    if req is not None and type is None:
        type = req.type
    if type is None or len(type.strip()) == 0:
        raise HTTPException(status_code=400, detail="type is required")
    state.current_refrigerant = type
    persist_selected_runtime()
    return {"message": "OK"}


class MeteringUpdate(BaseModel):
    id_txv: int

@app.post("/api/metering")
async def update_metering(
    req: Optional[MeteringUpdate] = None,
    id_txv: Optional[int] = Query(default=None),
    edge_id: Optional[str] = Query(default=None),
):
    maybe_select_edge(edge_id)
    if req is not None and id_txv is None:
        id_txv = req.id_txv
    if id_txv is None:
        raise HTTPException(status_code=400, detail="id_txv is required")
    state.id_is_txv = (id_txv == 1)
    persist_selected_runtime()
    return {"message": "OK"}


@app.get("/api/auth/check")
async def auth_check():
    if state.authRole in {"admin", "instructor", "student"}:
        return Response(content=state.authRole, media_type="text/plain")
    return Response(content="DENIED", media_type="text/plain", status_code=401)


@app.post("/api/users/add")
async def add_user(
    user: str = Form(...),
    passw: str = Form(..., alias="pass"),
    role: str = Form(...),
):
    require_instructor_or_admin()

    username = (user or "").strip()
    password = (passw or "").strip()
    normalized_role = (role or "").strip().lower()

    if len(username) < 3 or len(username) > 64:
        raise HTTPException(status_code=400, detail="Username must be 3-64 characters")
    if len(password) < 3 or len(password) > 128:
        raise HTTPException(status_code=400, detail="Password must be 3-128 characters")
    if normalized_role not in {"student", "instructor"}:
        raise HTTPException(status_code=400, detail="Role must be student or instructor")
    if any(existing.lower() == username.lower() for existing in users_db.keys()):
        raise HTTPException(status_code=409, detail="User already exists")

    users_db[username] = {
        "pw_hash": hash_password(password),
        "role": normalized_role,
    }
    save_users_db()

    return {
        "message": "OK",
        "user": username,
        "role": normalized_role,
    }

@app.get("/api/login")
async def login(user: str, passw: Optional[str] = None, pass_alias: Optional[str] = Query(default=None, alias="pass")):
    password = passw if passw is not None else pass_alias

    username = (user or "").strip()
    submitted = (password or "").strip()
    record = users_db.get(username)

    if record and verify_password(submitted, record):
        if "pw_hash" not in record and "pw" in record:
            record["pw_hash"] = hash_password(submitted)
            record.pop("pw", None)
            save_users_db()
        role = str(record.get("role", "student"))
        if role not in {"admin", "instructor", "student"}:
            role = "student"
        state.authRole = role
        return Response(content=f'{{"status":"success","role":"{role}","token":"mocktoken123"}}', media_type="application/json")

    state.authRole = "none"
    return Response(content='{"status":"denied"}', media_type="application/json", status_code=401)

@app.post("/api/logout")
async def logout():
    state.authRole = "none"
    return Response(content="OK", media_type="text/plain")

@app.post("/api/edge/heartbeat")
async def edge_heartbeat(req: Dict[str, Any], request: Request):
    now = time.time()
    source_ip = req.get("source_ip") or (request.client.host if request.client else "unknown")
    edge_id = str(req.get("edge_id") or source_ip or "unknown").strip()
    trainer_type = normalize_trainer_type(req.get("trainer_type"))
    mac_address = req.get("mac", "").lower()

    # --- IDENTITY RESOLUTION ---
    # If a device sends its MAC, it's asking for its proper identity. Find it by iterating.
    if mac_address:
        found_edge = None
        for edge in state.edges.values():
            if edge.get("mac") == mac_address:
                found_edge = edge
                break
        
        if found_edge:
            registered_id = found_edge.get("edge_id")
            registered_label = found_edge.get("label")
            if registered_id and registered_label and edge_id != registered_id and mqtt_client:
                command = {"action": "set_identity", "edge_id": registered_id, "label": registered_label}
                await mqtt_client.publish(f"trainer/{edge_id}/command", json.dumps(command), qos=1)
                print(f"Sent identity resolution to {edge_id} -> {registered_id}")

    label = str(req.get("device_name") or f"ESP32 {edge_id}").strip()

    prev = state.edges.get(edge_id, {})
    runtime = ensure_edge_runtime(prev) if isinstance(prev, dict) and prev else default_runtime()

    if req.get("w") is not None:
        runtime["state_w"] = bool(req.get("w"))
    if req.get("y") is not None:
        runtime["state_y"] = bool(req.get("y"))
    if req.get("g") is not None:
        runtime["state_g"] = bool(req.get("g"))
    if req.get("phys_lps") is not None:
        runtime["phys_lps_tripped"] = bool(req.get("phys_lps"))
    if req.get("phys_hps") is not None:
        runtime["phys_hps_tripped"] = bool(req.get("phys_hps"))

    core_keys = {
        "edge_id", "device_name", "source_ip", "w", "y", "g", "phys_lps", "phys_hps",
        "trainer_type", "wifi_rssi", "ram", "uptime", "temp"
    }
    prev_telemetry = prev.get("telemetry", {}) if isinstance(prev, dict) else {}
    telemetry = dict(prev_telemetry)
    for key, value in req.items():
        if key in core_keys:
            continue
        telemetry[key] = value

    state.edges[edge_id] = {
        "edge_id": edge_id,
        "label": label,
        "source_ip": source_ip,
        "last_seen": now,
        "trainer_type": trainer_type,
        "w": req.get("w") if req.get("w") is not None else prev.get("w", False),
        "y": req.get("y") if req.get("y") is not None else prev.get("y", False),
        "g": req.get("g") if req.get("g") is not None else prev.get("g", False),
        "phys_lps": req.get("phys_lps") if req.get("phys_lps") is not None else prev.get("phys_lps", False),
        "phys_hps": req.get("phys_hps") if req.get("phys_hps") is not None else prev.get("phys_hps", False),
        "wifi_rssi": req.get("wifi_rssi") if req.get("wifi_rssi") is not None else prev.get("wifi_rssi", -55),
        "ram": req.get("ram") if req.get("ram") is not None else prev.get("ram", 245760),
        "uptime": req.get("uptime") if req.get("uptime") else prev.get("uptime", "00:00:00"),
        "temp": req.get("temp") if req.get("temp") is not None else prev.get("temp", 98.0),
        "identity_reboot_required": req.get("identity_reboot_required", False),
        "mac": mac_address or prev.get("mac", ""),
        "telemetry": telemetry,
        "runtime": runtime,
    }

    if not state.selected_edge_id:
        state.selected_edge_id = edge_id

    if state.selected_edge_id == edge_id:
        sync_selected_edge_into_state()

    save_edges_db()

    return {
        "message": "OK",
        "edge_connected": 1,
        "edge_id": edge_id,
        "selected_edge_id": state.selected_edge_id,
    }


@app.get("/api/edges")
async def list_edges():
    return {
        "selected_edge_id": state.selected_edge_id,
        "edges": get_edges_payload(),
    }


class SelectEdgeRequest(BaseModel):
    edge_id: str


class RemoveEdgeRequest(BaseModel):
    edge_id: str


@app.post("/api/edges/select")
async def select_edge(req: SelectEdgeRequest):
    edge_id = (req.edge_id or "").strip()
    if not edge_id or edge_id not in state.edges:
        raise HTTPException(status_code=404, detail="Edge not found")

    if state.selected_edge_id and state.selected_edge_id != edge_id:
        persist_selected_runtime()
    state.selected_edge_id = edge_id
    sync_selected_edge_into_state()
    return {
        "message": "OK",
        "selected_edge_id": state.selected_edge_id,
    }


@app.post("/api/edges/remove")
async def remove_edge(req: RemoveEdgeRequest):
    require_instructor_or_admin()
    edge_id = (req.edge_id or "").strip()
    if not edge_id or edge_id not in state.edges:
        raise HTTPException(status_code=404, detail="Edge not found")

    was_selected = state.selected_edge_id == edge_id
    if was_selected:
        persist_selected_runtime()

    state.edges.pop(edge_id, None)

    if was_selected:
        state.selected_edge_id = None
        if state.edges:
            next_edge_id = sorted(state.edges.keys(), key=lambda k: k.lower())[0]
            state.selected_edge_id = next_edge_id
            sync_selected_edge_into_state()
        else:
            state.edge_connected = False

    save_edges_db(force=True)

    return {
        "message": "OK",
        "removed_edge_id": edge_id,
        "selected_edge_id": state.selected_edge_id,
        "edges": get_edges_payload(),
    }

@app.post("/api/edges/{edge_id}/command")
async def send_edge_command(edge_id: str, command: Dict[str, Any]):
    """Publish a command to a specific edge device via MQTT."""
    require_instructor_or_admin()
    if mqtt_client is None:
        raise HTTPException(status_code=503, detail="MQTT service is not available")
    if not edge_id or edge_id not in state.edges:
        raise HTTPException(status_code=404, detail="Edge device not found")
    
    topic = f"trainer/{edge_id}/command"
    payload = json.dumps(command)
    
    try:
        await mqtt_client.publish(topic, payload, qos=1)
        print(f"Sent command to {edge_id} on topic {topic}: {payload}")
        return {"message": "Command sent successfully", "edge_id": edge_id, "command": command}
    except MqttError as e:
        print(f"Failed to publish MQTT message to {topic}: {e}")
        raise HTTPException(status_code=500, detail=f"Failed to publish MQTT message: {e}")


@app.websocket("/ws")
async def websocket_status(websocket: WebSocket):
    await websocket.accept()
    active_websockets.append(websocket)
    try:
        while True:
            # We don't expect the client to send commands via WS in this build, but we must read to keep connection alive
            _ = await websocket.receive_text()
    except WebSocketDisconnect:
        if websocket in active_websockets:
            active_websockets.remove(websocket)
