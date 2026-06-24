#include <Arduino.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Wire.h>
#include <PCF8575.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <Adafruit_NeoPixel.h>
#include <DNSServer.h>
#include <Update.h>
#include <time.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <HTTPClient.h>

// ==========================================
// HARDWARE SETUP
// ==========================================
const int I2C_SDA_PIN = 8;
const int I2C_SCL_PIN = 9;

PCF8575 board_1(0x20, I2C_SDA_PIN, I2C_SCL_PIN); 
PCF8575 board_2(0x21, I2C_SDA_PIN, I2C_SCL_PIN);
PCF8575 board_3(0x22, I2C_SDA_PIN, I2C_SCL_PIN);

const int PIN_W = 7;
const int PIN_Y = 6;
const int PIN_G = 4;

Adafruit_NeoPixel status_led(1, 48, NEO_GRB + NEO_KHZ800);
uint32_t led_timer = 0;
bool ota_in_progress = false; 

DNSServer dnsServer;
bool is_ap_mode = false; 

// --- BLE RECONNECT STATE MACHINE ---
enum BleRestartState { BLE_IDLE, BLE_DISCONNECTED, BLE_RESTARTING };
BleRestartState ble_restart_state = BLE_IDLE;
uint32_t ble_restart_timer = 0;

// ==========================================
// GLOBAL TIMERS & STATES
// ==========================================
bool last_w_state = false; bool last_y_state = false; bool last_g_state = false;

uint32_t debounce_w = 0, debounce_y = 0, debounce_g = 0;
bool state_w = false, state_y = false, state_g = false;

// --- FURNACE STATE MACHINE ---
enum FurnaceState { FURNACE_IDLE, FURNACE_PRE_PURGE, FURNACE_IGNITER_WARMUP, FURNACE_TRIAL_FOR_IGNITION, FURNACE_HEATING, FURNACE_POST_PURGE, FURNACE_LOCKOUT };
FurnaceState furnace_state = FURNACE_IDLE;
uint32_t furnace_timer = 0;
uint32_t blower_on_delay = 0;
uint32_t blower_off_delay = 0;

bool inducer_on = false; bool igniter_on = false; bool gas_valve_on = false; bool heat_blower_on = false;

// --- DYNAMIC ELECTRICAL TELEMETRY ---
float sim_comp_amps = 0.0;
float sim_od_fan_amps = 0.0;
float sim_id_fan_amps = 0.0;
uint32_t comp_start_time = 0;
bool last_comp_state = false;

bool phys_lps_tripped = false;
bool phys_hps_tripped = false;

bool sim_active[16] = {false};
bool fault_active[55] = {false}; 

uint32_t sim_timer[16] = {0};   
int sim_step[16] = {0};         
int limit_trip_count = 0;       
String latest_diagnosis = "None";
int ignition_retry_count = 0;

// --- DYNAMIC REFRIGERANT PHYSICS ---
String current_refrigerant = "R410A"; 
bool force_pressure_snap = false;     
bool id_is_txv = true;

// --- DYNAMIC AMBIENT SLIDERS ---
float set_od_temp = 90.0;
float set_id_temp = 75.0;
float set_rh = 50.0; 

float sim_od_low_press = 145.0;
float sim_od_high_press = 145.0;
float sim_od_liquid_press = 145.0; // Fixed: Added raw float tracking variable for liquid gauge data stream
float sim_od_suction_temp = 90.0;
float sim_od_liquid_temp = 90.0;
float sim_od_discharge = 90.0;
float sim_od_ambient = 90.0;

float sim_id_ambient = 75.0; 
float sim_id_return_temp = 75.0;
float sim_id_supply_temp = 75.0;
float sim_id_rh = 50.0;

int active_scenario = 0;
uint32_t scenario_start_time = 0;
int student_score = 100;
String work_history_log = "";
uint32_t reset_counter = 0;
int total_score_sum = 0;
int completed_problems = 0;
int current_problem_score = 100;
String ble_login_status = "None"; // Tracks BLE login result ("success", "denied", or "None")
String wifi_ssid = "";
String wifi_pass = "";
String authToken = "";
String authRole = "";
uint32_t authExpiry = 0;
const uint32_t AUTH_TOKEN_TTL = 28800; // 8 hours

bool pending_reboot = false;
uint32_t reboot_timer = 0;

// --- DOCKER ENGINE LINK ---
// Update this IP to the machine running Docker (same LAN as ESP32).
String engine_base_url = "http://192.168.1.139:8000";
const char* EDGE_ID = "furnace_trainer_01";
const char* EDGE_LABEL = "Furnace Trainer 01";
const char* TRAINER_TYPE = "ac_gas";
const uint32_t ENGINE_HEARTBEAT_INTERVAL_MS = 200;
uint32_t engine_heartbeat_timer = 0;
bool engine_link_enabled = true;
bool hb_last_w = false;
bool hb_last_y = false;
bool hb_last_g = false;
bool hb_last_lps = false;
bool hb_last_hps = false;


void reset_all_faults_and_sims();
void handleDiagnosis(String submitted);
void sendEngineHeartbeat();

bool postEngineHeartbeat(const String& baseUrl, const String& payload) {
  if (baseUrl.length() == 0) return false;
  HTTPClient http;
  String endpoint = baseUrl + "/api/edge/heartbeat";
  http.setTimeout(1200);
  if (!http.begin(endpoint)) return false;

  http.addHeader("Content-Type", "application/json");
  int code = http.POST(payload);
  http.end();
  return code >= 200 && code < 300;
}

String generateAuthToken() {
  uint32_t valueA = esp_random();
  uint32_t valueB = esp_random();
  char buf[24];
  snprintf(buf, sizeof(buf), "%08X%08X", valueA, valueB);
  return String(buf);
}

String getAuthTokenFromRequest(AsyncWebServerRequest *request) {
  if (!request || !request->hasHeader("Cookie")) return String();
  const AsyncWebHeader* h = request->getHeader("Cookie");
  if (!h) return String();
  String cookie = h->value();
  int idx = cookie.indexOf("vesta_auth=");
  if (idx < 0) return String();
  int start = idx + 11;
  int end = cookie.indexOf(';', start);
  if (end < 0) end = cookie.length();
  return cookie.substring(start, end);
}

bool isAuthTokenValid(const String &token) {
  if (token.length() == 0 || authToken.length() == 0) return false;
  if (token != authToken) return false;
  if ((millis() / 1000) > authExpiry) {
    authToken = "";
    authRole = "";
    authExpiry = 0;
    return false;
  }
  return true;
}

bool isAuthenticatedRequest(AsyncWebServerRequest *request) {
  String token = getAuthTokenFromRequest(request);
  return isAuthTokenValid(token);
}

bool isAdminRequest(AsyncWebServerRequest *request) {
  if (!request) return false;
  if (!isAuthenticatedRequest(request)) return false;
  return authRole == "admin" || authRole == "instructor";
}

void clearAuth() {
  authToken = "";
  authRole = "";
  authExpiry = 0;
}

void set_furnace_relays(bool inducer, bool igniter, bool gas, bool blower) {
  inducer_on = inducer; igniter_on = igniter; gas_valve_on = gas; heat_blower_on = blower;
  board_2.digitalWrite(0, inducer ? LOW : HIGH);
  board_2.digitalWrite(1, igniter ? LOW : HIGH);
  board_2.digitalWrite(2, gas ? LOW : HIGH);
  board_2.digitalWrite(3, blower ? LOW : HIGH);
}

void initUserDatabase() {
  if (!LittleFS.exists("/users.json")) {
    String adminPw = generateAuthToken();
    File f = LittleFS.open("/users.json", FILE_WRITE);
    DynamicJsonDocument doc(1024);
    doc["admin"]["pw"] = adminPw;
    doc["admin"]["role"] = "instructor";
    serializeJson(doc, f);
    f.close();
    Serial.println("=== Generated initial admin credentials ===");
    Serial.printf("Username: admin\nPassword: %s\n", adminPw.c_str());
    Serial.println("========================================");
  }
}

void loadWiFiConfig() {
  if (LittleFS.exists("/wifi.json")) {
    File f = LittleFS.open("/wifi.json", FILE_READ);
    if (f) {
      DynamicJsonDocument doc(1024);
      DeserializationError error = deserializeJson(doc, f);
      f.close();
      if (!error && !doc["ssid"].isNull() && !doc["pass"].isNull()) {
        wifi_ssid = doc["ssid"].as<String>();
        wifi_pass = doc["pass"].as<String>();
      }
    }
  }
}

void sendEngineHeartbeat() {
  if (!engine_link_enabled) return;
  if (is_ap_mode) return;
  if (WiFi.status() != WL_CONNECTED) return;

  uint32_t now = millis();
  bool changed = (state_w != hb_last_w) ||
                 (state_y != hb_last_y) ||
                 (state_g != hb_last_g) ||
                 (phys_lps_tripped != hb_last_lps) ||
                 (phys_hps_tripped != hb_last_hps);

  if (!changed && now < engine_heartbeat_timer) return;
  engine_heartbeat_timer = now + ENGINE_HEARTBEAT_INTERVAL_MS;

  DynamicJsonDocument doc(256);
  uint32_t uptime_sec = millis() / 1000;
  char uptime_str[15];
  snprintf(uptime_str, sizeof(uptime_str), "%02d:%02d:%02d", (int)(uptime_sec / 3600), (int)((uptime_sec % 3600) / 60), (int)(uptime_sec % 60));

  doc["edge_id"] = EDGE_ID;
  doc["device_name"] = EDGE_LABEL;
  doc["w"] = state_w;
  doc["y"] = state_y;
  doc["g"] = state_g;
  doc["phys_lps"] = phys_lps_tripped;
  doc["phys_hps"] = phys_hps_tripped;
  doc["trainer_type"] = TRAINER_TYPE;
  doc["wifi_rssi"] = WiFi.RSSI();
  doc["ram"] = ESP.getFreeHeap();
  doc["uptime"] = uptime_str;
  doc["temp"] = 0.0;

  String payload;
  serializeJson(doc, payload);

  if (postEngineHeartbeat(engine_base_url, payload)) {
    hb_last_w = state_w;
    hb_last_y = state_y;
    hb_last_g = state_g;
    hb_last_lps = phys_lps_tripped;
    hb_last_hps = phys_hps_tripped;
    return;
  }

  String gatewayBase = "http://" + WiFi.gatewayIP().toString() + ":8000";
  if (gatewayBase != engine_base_url && postEngineHeartbeat(gatewayBase, payload)) {
    engine_base_url = gatewayBase;
    hb_last_w = state_w;
    hb_last_y = state_y;
    hb_last_g = state_g;
    hb_last_lps = phys_lps_tripped;
    hb_last_hps = phys_hps_tripped;
    Serial.printf("Engine heartbeat switched to gateway endpoint: %s\n", engine_base_url.c_str());
    return;
  }

  Serial.printf("Engine heartbeat failed at %s\n", engine_base_url.c_str());
}

AsyncWebServer server(80);
AsyncWebSocket ws("/ws"); 

bool readDebounced(int pin, bool &stable_state, uint32_t &timer) {
  bool raw_state = !digitalRead(pin); 
  if (raw_state != stable_state) {
    if (timer == 0) timer = millis();
    else if (millis() - timer > 150) { 
      stable_state = raw_state;
      timer = 0;
    }
  } else { timer = 0; }
  return stable_state;
}

String getExpectedDiagnosis() {
  if (fault_active[46]) return "Low Indoor Airflow";
  if (fault_active[47]) return "High Indoor Airflow";
  if (fault_active[40]) return "Non-Condensables";
  if (fault_active[41]) return "Stuck Indoor TXV";
  if (fault_active[42]) return "Clogged TXV";
  if (fault_active[43]) return "Clogged Piston";;
  if (fault_active[44]) return "Compressor Internal Bypass";
  if (fault_active[45]) return "Inefficient Compressor";
  
  if (fault_active[24] || sim_active[1] || sim_active[6]) return "Failed Indoor Blower";
  if (fault_active[6] || sim_active[3]) return "Failed Condenser Fan";
  if (sim_active[15] || fault_active[31] || fault_active[4]) return "Failed Compressor / Overload";
  if (fault_active[15]) return "Failed Inducer Motor";
  if (fault_active[18]) return "Failed Hot Surface Igniter";
  if (fault_active[21]) return "Stuck Gas Valve (Closed)";
  if (fault_active[20]) return "Stuck Gas Valve (Open)";
  if (fault_active[32]) return "Dirty Flame Sensor";
  if (fault_active[16]) return "Pressure Switch Stuck Open";
  if (fault_active[19]) return "Pressure Switch Stuck Closed";
  if (fault_active[22]) return "Open High Limit Switch";
  if (fault_active[33]) return "Open Rollout Switch";

  bool any_fault = false;
  for(int i=0; i<55; i++) if(fault_active[i]) any_fault = true;
  for(int i=0; i<16; i++) if(sim_active[i]) any_fault = true;
  if (!any_fault) return "Normal Operation";

  return "Unknown Fault";
}

String getStatusJSON() {
  uint32_t uptime_sec = millis() / 1000;
  char uptime_str[15];
  snprintf(uptime_str, sizeof(uptime_str), "%02d:%02d:%02d", (int)(uptime_sec / 3600), (int)((uptime_sec % 3600) / 60), (int)(uptime_sec % 60));

  float low_noise = (esp_random() % 100) / 100.0 * 2.0 - 1.0;
  float high_noise = (esp_random() % 100) / 100.0 * 4.0 - 2.0;
  float liquid_noise = (esp_random() % 100) / 100.0 * 3.0 - 1.5;

  DynamicJsonDocument doc(8192);
  
  doc["w_call"] = state_w ? true : false;
  doc["y_call"] = state_y ? true : false;
  doc["g_call"] = state_g ? true : false;
  
  doc["wifi_rssi"] = is_ap_mode ? 0 : WiFi.RSSI();
  doc["ram"] = ESP.getFreeHeap();
  doc["uptime"] = uptime_str;
  doc["clients"] = ws.count();
  doc["diagnosis"] = latest_diagnosis;
  doc["student_score"] = student_score;
  doc["work_history"] = work_history_log;
  doc["refrigerant"] = current_refrigerant;
  doc["id_is_txv"] = id_is_txv ? 1 : 0; 
  doc["indoor_metering"] = id_is_txv ? "TXV" : "Piston";
  
  doc["set_od"] = round(set_od_temp * 10.0) / 10.0;
  doc["set_id"] = round(set_id_temp * 10.0) / 10.0;
  doc["set_rh"] = round(set_rh * 10.0) / 10.0; 
  
  doc["phys_lps"] = phys_lps_tripped ? 1 : 0;
  doc["phys_hps"] = phys_hps_tripped ? 1 : 0;
  doc["lps_open"] = (phys_lps_tripped || fault_active[7] || fault_active[26]) ? 1 : 0;
  doc["hps_open"] = (phys_hps_tripped || fault_active[8] || fault_active[27]) ? 1 : 0;
  
  doc["od_low_press"] = round((sim_od_low_press + low_noise) * 10.0) / 10.0;
  doc["od_high_press"] = round((sim_od_high_press + high_noise) * 10.0) / 10.0;
  doc["od_liquid_press"] = round((sim_od_liquid_press + liquid_noise) * 10.0) / 10.0; // Fixed: Broadcast separate liquid pressure channel variable over WebSockets
  doc["od_suction_temp"] = round(sim_od_suction_temp * 10.0) / 10.0;
  doc["od_liquid_temp"] = round(sim_od_liquid_temp * 10.0) / 10.0;
  doc["od_ambient"] = round(sim_od_ambient * 10.0) / 10.0;
  
  doc["id_return_temp"] = round(sim_id_return_temp * 10.0) / 10.0;
  doc["id_supply_temp"] = round(sim_id_supply_temp * 10.0) / 10.0;
  doc["id_ambient"] = round(sim_id_ambient * 10.0) / 10.0; 
  doc["id_rh"] = round(sim_id_rh * 10.0) / 10.0;
  doc["id_suction_temp"] = round((sim_od_suction_temp + 1.2) * 10.0) / 10.0; 
  doc["id_liquid_temp"] = round((sim_od_liquid_temp - 1.2) * 10.0) / 10.0;        
  
  doc["comp_amps"] = round(sim_comp_amps * 10.0) / 10.0;
  doc["od_fan_amps"] = round(sim_od_fan_amps * 10.0) / 10.0;
  doc["id_fan_amps"] = round(sim_id_fan_amps * 10.0) / 10.0;
  doc["hs_amps"] = round(heat_blower_on ? (inducer_on ? 1.2 : 0) + (igniter_on ? 3.5 : 0) + (gas_valve_on ? 0.5 : 0) : 0.0);

  doc["active_scenario"] = active_scenario;
  doc["student_score"] = student_score;
  doc["reset_counter"] = reset_counter;
  doc["ble_login_status"] = ble_login_status;
  doc["trainer_type"] = TRAINER_TYPE;

  // Export full fault/simulation bitfields so instructor UIs can mirror every toggle state.
  for (int faultIdx = 1; faultIdx < 55; faultIdx++) {
    char key[8];
    snprintf(key, sizeof(key), "f%d", faultIdx);
    doc[key] = fault_active[faultIdx] ? 1 : 0;
  }

  for (int simIdx = 1; simIdx < 16; simIdx++) {
    char key[10];
    snprintf(key, sizeof(key), "sim_%02d", simIdx);
    doc[key] = sim_active[simIdx] ? 1 : 0;
  }

  // Relay state exports to match common instructor control IDs.
  doc["relay_inducer"] = inducer_on ? 1 : 0;
  doc["relay_igniter"] = igniter_on ? 1 : 0;
  doc["relay_gas_valve"] = gas_valve_on ? 1 : 0;
  doc["relay_heat_blower"] = heat_blower_on ? 1 : 0;
  // Furnace relay aliases backed by existing fault lines.
  doc["relay_rollout_limit_1"] = fault_active[33] ? 1 : 0;
  doc["relay_rollout_limit_2"] = fault_active[34] ? 1 : 0;
  doc["relay_high_temp_limit"] = fault_active[22] ? 1 : 0;
  doc["relay_flame_sensor"] = fault_active[32] ? 1 : 0;
  doc["relay_condenser_fan"] = fault_active[6] ? 1 : 0;
  doc["relay_low_pressure_switch"] = fault_active[7] ? 1 : 0;
  doc["relay_high_pressure_switch"] = fault_active[8] ? 1 : 0;
  doc["relay_vacuum_pressure_switch"] = fault_active[16] ? 1 : 0;
  doc["relay_pressure_switch_closed"] = fault_active[19] ? 1 : 0;
  doc["relay_inducer_open"] = fault_active[15] ? 1 : 0;
  doc["relay_igniter_open"] = fault_active[18] ? 1 : 0;
  doc["relay_gas_valve_closed"] = fault_active[21] ? 1 : 0;
  doc["relay_gas_valve_open"] = fault_active[20] ? 1 : 0;
  doc["relay_draft_safeguard"] = fault_active[17] ? 1 : 0;
  doc["relay_blocked_flue"] = fault_active[23] ? 1 : 0;
  doc["relay_indoor_fan_off"] = fault_active[24] ? 1 : 0;
  doc["relay_indoor_fan_on"] = fault_active[25] ? 1 : 0;
  doc["relay_shorted_contactor"] = fault_active[30] ? 1 : 0;
  doc["relay_comp_limit_open"] = fault_active[31] ? 1 : 0;
  doc["relay_low_pressure_board_fault"] = fault_active[26] ? 1 : 0;
  doc["relay_high_pressure_board_fault"] = fault_active[27] ? 1 : 0;
  doc["relay_failed_gas_relay"] = fault_active[34] ? 1 : 0;
  doc["relay_a2l_sensor"] = fault_active[28] ? 1 : 0;
  doc["relay_a2l_board_fault"] = fault_active[29] ? 1 : 0;
  doc["relay_failed_gas_valve"] = fault_active[21] ? 1 : 0;
  doc["relay_faulty_high_temp_limit"] = fault_active[22] ? 1 : 0;
  doc["relay_grounded_w_wire"] = fault_active[52] ? 1 : 0;
  doc["relay_shorted_y_to_r"] = fault_active[51] ? 1 : 0;
  doc["relay_shorted_w_to_r"] = fault_active[53] ? 1 : 0;
  doc["relay_draft_inducer_board_fault"] = fault_active[17] ? 1 : 0;
  doc["relay_shorted_ignition_board"] = fault_active[18] ? 1 : 0;
  doc["relay_faulty_a2l_sensor"] = fault_active[28] ? 1 : 0;
  doc["relay_faulty_a2l_board"] = fault_active[29] ? 1 : 0;
  doc["relay_open_contactor_coil"] = fault_active[31] ? 1 : 0;
  doc["relay_shorted_contactor_coil"] = fault_active[54] ? 1 : 0;
  if (ble_login_status == "success" && authToken.length() > 0) {
    doc["auth_token"] = authToken;
  }

  doc["furnace_state"] = (int)furnace_state;
  doc["inducer_on"] = inducer_on ? 1 : 0;
  doc["igniter_on"] = igniter_on ? 1 : 0;
  doc["gas_valve_on"] = gas_valve_on ? 1 : 0;
  doc["heat_blower_on"] = heat_blower_on ? 1 : 0;

  float temp_c = temperatureRead();
  float temp_f = (temp_c * 9.0 / 5.0) + 32.0;
  doc["temp"] = temp_f;

  String json;
  serializeJson(doc, json);
  return json;
}

// ==========================================
// BLE COMMUNICATION
// ==========================================
#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

NimBLEServer* pServer = nullptr;
NimBLECharacteristic* pTxCharacteristic = nullptr;
volatile bool deviceConnected = false;
volatile bool oldDeviceConnected = false;
volatile bool force_telemetry_update = false;
volatile int negotiated_mtu = 20;

// ==========================================
// THROTTELED & SAFE BLE NOTIFY CHANNELS
// ==========================================
uint32_t ble_timer = 0;
void notifyClients(bool high_priority = false) { 
  String json = getStatusJSON();
  bool delivered = false;
  
  // 1. Send WebSocket over Wi-Fi
  if (ws.count() > 0) {
    ws.textAll(json); 
    delivered = true;
  }
  
  // 2. Send BLE Telemetry (Throttled to 1.5s to avoid RF antenna congestion)
  uint32_t now = millis();
  if (deviceConnected && pTxCharacteristic && (high_priority || (now - ble_timer >= 1500))) {
    if (high_priority || (now - ble_timer >= 1500)) { 
      ble_timer = now;
      int len = json.length();
      int offset = 0;
      while (offset < len) {
        int chunkSize = (len - offset < negotiated_mtu) ? (len - offset) : negotiated_mtu;
        pTxCharacteristic->setValue(json.substring(offset, offset + chunkSize).c_str());
        pTxCharacteristic->notify();
        offset += chunkSize;
      }
      delivered = true;
    }
  }

  // One-shot status events (like BLE login result) should only clear after real delivery.
  if (delivered && ble_login_status != "None") {
    ble_login_status = "None";
  }
}

class MyServerCallbacks: public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    deviceConnected = true;
    negotiated_mtu = connInfo.getMTU() - 3;
    if (negotiated_mtu < 20) negotiated_mtu = 20;
  }
  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    deviceConnected = false;
    negotiated_mtu = 20;
  }
  void onMTUChange(uint16_t MTU, NimBLEConnInfo& connInfo) override {
    negotiated_mtu = MTU - 3;
    if (negotiated_mtu < 20) negotiated_mtu = 20;
  }
};

void logLogin(String username, String role); // Forward declaration

bool checkCredentials(String user, String pass) {
  user.toLowerCase();
  File f = LittleFS.open("/users.json", FILE_READ);
  if (!f) return false;
  DynamicJsonDocument db(2048);
  DeserializationError error = deserializeJson(db, f);
  f.close();
  if (!error && !db[user].isNull() && db[user]["pw"].as<String>() == pass) {
    return true;
  }
  return false;
}

String getUserRole(String user) {
  user.toLowerCase();
  File f = LittleFS.open("/users.json", FILE_READ);
  if (!f) return "student";
  DynamicJsonDocument db(2048);
  DeserializationError error = deserializeJson(db, f);
  f.close();
  if (!error && !db[user].isNull() && !db[user]["role"].isNull()) {
    return db[user]["role"].as<String>();
  }
  return "student";
}

class MyCallbacks: public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo& connInfo) override {
    std::string rxValue = pCharacteristic->getValue();
    if (rxValue.length() > 0) {
      Serial.printf("BLE Received Payload: %s\n", rxValue.c_str());

      DynamicJsonDocument doc(2048);
      DeserializationError error = deserializeJson(doc, rxValue.c_str());

      if (!error) {
        if (!doc["reset_score"].isNull()) { //
          student_score = 100;
          work_history_log = "";
          latest_diagnosis = "None";
          Serial.println("Reset Score Command Received over BLE");
        }
        if (!doc["diagnosis"].isNull()) {
          String submitted = doc["diagnosis"].as<String>();
          Serial.printf("Submitting Diagnosis over BLE: %s\n", submitted.c_str());
          handleDiagnosis(submitted);
        }
        
        // --- NEW: BLE USER LOGIN CHECK ---
        if (!doc["user"].isNull() && !doc["pass"].isNull()) { //
          String u = doc["user"].as<String>();
          String p = doc["pass"].as<String>();
          Serial.printf("Checking BLE Login Credentials for user: %s\n", u.c_str());
          if (checkCredentials(u, p)) {
            authToken = generateAuthToken();
            authRole = getUserRole(u);
            authExpiry = (millis() / 1000) + AUTH_TOKEN_TTL;
            ble_login_status = "success";
            logLogin(u, authRole);
            Serial.printf("BLE Authentication SUCCESS! role=%s\n", authRole.c_str());
          } else {
            ble_login_status = "denied";
            Serial.println("BLE Authentication DENIED!");
          }
          // Do NOT call notifyClients() here — we are inside a BLE callback task.
          // Setting force_telemetry_update will cause the main loop to send the response safely.
          force_telemetry_update = true;
        }
        
        force_telemetry_update = true;
      } else {
        Serial.printf("BLE JSON Deserialize Error: %s\n", error.c_str());
      }
    }
  }
};

void setupBLE() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char bleName[25];
  sprintf(bleName, "VestaCore%02X%02X", mac[4], mac[5]);
  
  NimBLEDevice::init(bleName);
  NimBLEDevice::setMTU(512); // Request maximum MTU for faster/larger payload transfers
  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  NimBLEService *pService = pServer->createService(SERVICE_UUID);
  pTxCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID_TX, NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic *pRxCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID_RX, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  pRxCharacteristic->setCallbacks(new MyCallbacks());
  
  NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->enableScanResponse(true); 
  pAdvertising->start();
}

void reset_all_faults_and_sims() {
  active_scenario = 0;
  scenario_start_time = 0;

  phys_lps_tripped = false;
  phys_hps_tripped = false;
  
  comp_start_time = 0;
  last_comp_state = false;

  for(int i = 0; i < 16; i++) { sim_active[i] = false; sim_timer[i] = 0; sim_step[i] = 0; }
  for(int i = 0; i < 55; i++) { fault_active[i] = false; } 
  
  limit_trip_count = 0;

  furnace_state = FURNACE_IDLE; furnace_timer = 0; blower_on_delay = 0; blower_off_delay = 0;
  set_furnace_relays(false, false, false, false);

  for (int i = 0; i < 16; i++) {
    board_1.digitalWrite(i, HIGH);
    board_2.digitalWrite(i, HIGH);
    board_3.digitalWrite(i, HIGH);
    yield(); // Prevent watchdog starvation during 48 blocking I2C writes
  }
  reset_counter++;
}

void logLogin(String username, String role) {
  struct tm timeinfo;
  String entry = "";
  if(getLocalTime(&timeinfo)){
    char timeStr[64];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
    entry = String(timeStr) + " | " + username + " logged in as " + role;
  } else {
    entry = "Time Syncing... | " + username + " logged in as " + role;
  }
  
  File logFile = LittleFS.open("/access.log", FILE_APPEND);
  if (logFile) {
    logFile.println(entry);
    logFile.close();
  }
}

void handleDiagnosis(String submitted) {
  String expected = getExpectedDiagnosis();
  
  bool correct = false;
  if (expected == "Unknown Fault" && submitted != "Normal Operation") {
      correct = true; 
  } else if (submitted == expected) {
      correct = true;
  }

  struct tm timeinfo;
  String timeStr = "Now";
  if(getLocalTime(&timeinfo)){
      char tStr[64];
      strftime(tStr, sizeof(tStr), "%H:%M:%S", &timeinfo);
      timeStr = String(tStr);
  }

  if (correct) {
      latest_diagnosis = "CORRECT: " + submitted;
      work_history_log += "<span style=\"color: #10b981;\">" + timeStr + " - Correct: " + submitted + " (Score: " + String(current_problem_score) + ")</span><br>";
      
      total_score_sum += current_problem_score;
      completed_problems++;
      current_problem_score = 100; 
      student_score = (total_score_sum + current_problem_score) / (completed_problems + 1);

      reset_all_faults_and_sims();
  } else {
      latest_diagnosis = "INCORRECT: " + submitted;
      
      current_problem_score -= 10;
      if (current_problem_score < 0) current_problem_score = 0;
      student_score = (total_score_sum + current_problem_score) / (completed_problems + 1);
      
      work_history_log += "<span style=\"color: #ef4444;\">" + timeStr + " - Incorrect: " + submitted + "</span><br>";
  }
  force_telemetry_update = true;
}

void setup() {
  Serial.begin(115200);
  status_led.begin(); status_led.setBrightness(100);
  status_led.setPixelColor(0, status_led.Color(200, 100, 0)); status_led.show();

  setupBLE(); // Initialize BLE early to secure memory and RF coexistence before WiFi

  if(!LittleFS.begin(true)) { Serial.println("LittleFS Mount Failed"); return; }
  initUserDatabase();

  loadWiFiConfig();
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
  int attempts = 0;
   while (WiFi.status() != WL_CONNECTED && attempts < 20) { delay(500); attempts++; }

  if (WiFi.status() == WL_CONNECTED) {
    is_ap_mode = false;
    Serial.println("\nWiFi connected.");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    Serial.printf("Engine endpoint: %s\n", engine_base_url.c_str());
    if (MDNS.begin("trainer2")) {
      Serial.println("MDNS responder started! Domain: trainer2.local");
      MDNS.addService("http", "tcp", 80); 
    }
    configTzTime("EST5EDT,M3.2.0,M11.1.0", "pool.ntp.org", "time.nist.gov");
  } else {
    is_ap_mode = true;
    const char* apPass = "8037945526";
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Vesta Core Trainer", apPass);
    Serial.printf("SoftAP password: %s\n", apPass);
    dnsServer.start(53, "*", WiFi.softAPIP()); 
  }
  
  ArduinoOTA.setHostname("trainer2");
  ArduinoOTA.onStart([]() {
    ota_in_progress = true;
    status_led.setPixelColor(0, status_led.Color(0, 0, 255)); // Solid blue
    status_led.show();
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    ota_in_progress = false;
    status_led.setPixelColor(0, status_led.Color(0, 255, 0)); // Green on success
    status_led.show();
    Serial.println("\nEnd");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    ota_in_progress = false;
    status_led.setPixelColor(0, status_led.Color(255, 0, 0)); // Red on error
    status_led.show();
    Serial.printf("OTA Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    uint8_t blue_val = 50 + ((progress * 205) / total); // Ramps brightness dynamically from dim to max
    status_led.setPixelColor(0, status_led.Color(0, 0, blue_val)); status_led.show();
  });
  ArduinoOTA.begin();

  pinMode(PIN_W, INPUT_PULLUP); pinMode(PIN_Y, INPUT_PULLUP); pinMode(PIN_G, INPUT_PULLUP);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  board_1.begin(); board_2.begin(); board_3.begin();
  reset_all_faults_and_sims();

  ws.onEvent([](AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) { client->text(getStatusJSON()); }
  });
  server.addHandler(&ws);

  // --- OTA Update via Web Browser ---
  server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", 
      "<form method='POST' action='/update' enctype='multipart/form-data'>"
      "<input type='file' name='update'>"
      "<input type='submit' value='Update'>"
      "</form>");
  });
  server.on("/update", HTTP_POST, [](AsyncWebServerRequest *request){
    AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
    response->addHeader("Connection", "close");
    request->send(response);
    if (!Update.hasError()) {
        delay(1000);
        ESP.restart();
    }
  }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
    if (!index) {
      ota_in_progress = true;
      status_led.setPixelColor(0, status_led.Color(0, 0, 255)); status_led.show();
      Serial.printf("Update Start: %s\n", filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { Update.printError(Serial); }
    }
    if (len) { if (Update.write(data, len) != len) { Update.printError(Serial); } }
    if (final) {
      ota_in_progress = false;
      if (Update.end(false)) {
        status_led.setPixelColor(0, status_led.Color(0, 255, 0)); status_led.show();
        Serial.printf("Update Success: %uB\n", index + len);
      } else {
        status_led.setPixelColor(0, status_led.Color(255, 0, 0)); status_led.show();
        Update.printError(Serial);
      }
    }
  });

  server.on("/api/metering", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!isAuthenticatedRequest(request)) { request->send(401, "text/plain", "DENIED"); return; }
    if (request->hasParam("id_txv")) { id_is_txv = (request->getParam("id_txv")->value() == "1"); }
    force_telemetry_update = true; 
    request->send(200, "text/plain", "OK");
  });

  server.on("/api/refrigerant", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!isAuthenticatedRequest(request)) { request->send(401, "text/plain", "DENIED"); return; }
    if (request->hasParam("type")) {
      current_refrigerant = request->getParam("type")->value();
      force_pressure_snap = true; 
      force_telemetry_update = true; 
      request->send(200, "text/plain", "OK");
    } else { request->send(400, "text/plain", "Bad Request"); }
  });

  server.on("/api/users/add", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!isAdminRequest(request)) { request->send(401, "text/plain", "DENIED"); return; }

    // Robust path for web form posts (avoids chunked JSON body edge cases).
    if (request->hasParam("user", true) && request->hasParam("pass", true) && request->hasParam("role", true)) {
      String newUser = request->getParam("user", true)->value();
      String newPass = request->getParam("pass", true)->value();
      String newRole = request->getParam("role", true)->value();

      newUser.trim();
      newPass.trim();
      newRole.trim();
      newUser.toLowerCase();
      newRole.toLowerCase();

      if (newUser.length() == 0 || newPass.length() == 0 || (newRole != "student" && newRole != "instructor" && newRole != "admin")) {
        request->send(400, "text/plain", "Bad Request");
        return;
      }

      File f = LittleFS.open("/users.json", FILE_READ);
      DynamicJsonDocument db(2048);
      deserializeJson(db, f);
      f.close();

      db[newUser]["pw"] = newPass;
      db[newUser]["role"] = newRole;

      f = LittleFS.open("/users.json", FILE_WRITE);
      serializeJson(db, f);
      f.close();

      logLogin("SYSTEM", "Created/Updated Profile: " + newUser);
      request->send(200, "text/plain", "User Encoded");
      return;
    }
  }, NULL,
  [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
    if (!isAdminRequest(request)) { request->send(401, "text/plain", "DENIED"); return; }
    DynamicJsonDocument doc(len + 512);
    DeserializationError error = deserializeJson(doc, data, len); //
    if (!error && !doc["user"].isNull() && !doc["pass"].isNull()) {
      File f = LittleFS.open("/users.json", FILE_READ);
      DynamicJsonDocument db(2048); deserializeJson(db, f); f.close();
      String newUser = doc["user"].as<String>(); newUser.toLowerCase();
      db[newUser]["pw"] = doc["pass"].as<String>(); 
      db[newUser]["role"] = doc["role"].as<String>();
      f = LittleFS.open("/users.json", FILE_WRITE); serializeJson(db, f); f.close();
      logLogin("SYSTEM", "Created/Updated Profile: " + newUser);
      request->send(200, "text/plain", "User Encoded");
    } else {
      request->send(400, "text/plain", "Bad Request");
    }
  });

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    AsyncWebServerResponse *response = request->beginResponse(LittleFS, "/index.html", "text/html");
    response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    request->send(response);
  });

  server.on("/api/login", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("user") && request->hasParam("pass")) {
      String user = request->getParam("user")->value();
      String pass = request->getParam("pass")->value();
      user.toLowerCase();
      File f = LittleFS.open("/users.json", FILE_READ);
      DynamicJsonDocument db(2048); DeserializationError error = deserializeJson(db, f); f.close();
      if (!error && !db[user].isNull() && db[user]["pw"].as<String>() == pass) {
        String role = db[user]["role"].as<String>();
        authToken = generateAuthToken();
        authRole = role;
        authExpiry = (millis() / 1000) + AUTH_TOKEN_TTL;
        logLogin(user, role);
        AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", role);
        response->addHeader("Set-Cookie", String("vesta_auth=") + authToken + "; Max-Age=" + AUTH_TOKEN_TTL + "; Path=/; SameSite=Lax; HttpOnly");
        request->send(response);
      } else { request->send(401, "text/plain", "DENIED"); }
    } else { request->send(400, "text/plain", "Bad Request"); }
  });

  server.on("/api/auth/check", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!isAuthenticatedRequest(request)) { request->send(401, "text/plain", "DENIED"); return; }
    request->send(200, "text/plain", authRole);
  });

  server.on("/api/logout", HTTP_POST, [](AsyncWebServerRequest *request){
    clearAuth();
    AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", "OK");
    response->addHeader("Set-Cookie", "vesta_auth=; Max-Age=0; Path=/; SameSite=Lax; HttpOnly");
    request->send(response);
  });

  server.on("/wifi-setup", HTTP_GET, [](AsyncWebServerRequest *request){
    String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'></head>"
                  "<body style='font-family: sans-serif; padding: 20px;'>"
                  "<h2>Wi-Fi Setup</h2>"
                  "<form action='/wifi-save' method='POST'>"
                  "<label>SSID:</label><br><input type='text' name='ssid' value='" + wifi_ssid + "' style='width:100%; max-width:300px;'><br><br>"
                  "<label>Password:</label><br><input type='password' name='pass' style='width:100%; max-width:300px;'><br><br>"
                  "<input type='submit' value='Save & Reboot' style='padding: 10px 20px;'></form>"
                  "</body></html>";
    request->send(200, "text/html", html);
  });

  server.on("/wifi-save", HTTP_POST, [](AsyncWebServerRequest *request){
    if(request->hasParam("ssid", true) && request->hasParam("pass", true)) {
      DynamicJsonDocument doc(512);
      doc["ssid"] = request->getParam("ssid", true)->value();
      doc["pass"] = request->getParam("pass", true)->value();
      File f = LittleFS.open("/wifi.json", FILE_WRITE); serializeJson(doc, f); f.close();
      request->send(200, "text/html", "<html><body style='font-family:sans-serif;'><h2>Saved! Device rebooting...</h2></body></html>");
      pending_reboot = true; reboot_timer = millis() + 2000;
    } else { request->send(400, "text/plain", "Missing credentials"); }
  });

  server.on("/student", HTTP_GET, [](AsyncWebServerRequest *request){
    AsyncWebServerResponse *response = request->beginResponse(LittleFS, "/student.html", "text/html");
    response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    request->send(response);
  });
  
  server.on("/instructor", HTTP_GET, [](AsyncWebServerRequest *request){
    AsyncWebServerResponse *response = request->beginResponse(LittleFS, "/instructor.html", "text/html");
    response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    request->send(response);
  });

  server.on("/access.log", HTTP_GET, [](AsyncWebServerRequest *request){
    if (LittleFS.exists("/access.log")) { request->send(LittleFS, "/access.log", "text/plain"); } 
    else { request->send(404, "text/plain", "Log file is empty."); }
  });

  server.on("/api/reset", HTTP_POST, [](AsyncWebServerRequest *request){ if(!isAdminRequest(request)) { request->send(401, "text/plain", "DENIED"); return; } reset_all_faults_and_sims(); force_telemetry_update = true; request->send(200, "text/plain", "OK"); });

  server.on("/api/submit", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!isAuthenticatedRequest(request)) { request->send(401, "text/plain", "DENIED"); return; }
    if (request->hasParam("diagnosis")) { 
      String submitted = request->getParam("diagnosis")->value();
      handleDiagnosis(submitted);
      if (latest_diagnosis.startsWith("CORRECT")) {
          request->send(200, "text/plain", "CORRECT");
      } else {
          request->send(200, "text/plain", "INCORRECT");
      }
    } else { request->send(400, "text/plain", "Bad Request"); }
  });
  
  server.on("/api/ambient", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!isAuthenticatedRequest(request)) { request->send(401, "text/plain", "DENIED"); return; }
    if (request->hasParam("od") && request->hasParam("id")) {
      set_od_temp = request->getParam("od")->value().toFloat();
      set_id_temp = request->getParam("id")->value().toFloat();
      if(request->hasParam("rh")) set_rh = request->getParam("rh")->value().toFloat(); 
      force_telemetry_update = true; 
      request->send(200, "text/plain", "OK");
    } else { request->send(400, "text/plain", "Bad Request"); }
  });

  server.onNotFound([](AsyncWebServerRequest *request){
    if (request->method() == HTTP_OPTIONS) {
      request->send(200);
    } else if (is_ap_mode) { 
      request->redirect("http://" + WiFi.softAPIP().toString() + "/"); 
    } else { 
      request->send(404, "text/plain", "Not Found"); 
    }
  });

  server.on("/api/toggle", HTTP_POST, [](AsyncWebServerRequest *request){
    if(!isAdminRequest(request)) { request->send(401, "text/plain", "DENIED"); return; }
    if (request->hasParam("id") && request->hasParam("state")) {
      String id = request->getParam("id")->value();
      bool state = request->getParam("state")->value().toInt() == 1;
      int val_inv = state ? LOW : HIGH; int val_nor = state ? HIGH : LOW; 
      
      if (id == "reset_score") {
          student_score = 100;
          work_history_log = "";
          latest_diagnosis = "None";
      } else if (id.startsWith("sim_")) {
        int simNum = id.substring(4).toInt();
        if (simNum >= 1 && simNum <= 15) { sim_active[simNum] = state; sim_timer[simNum] = 0; sim_step[simNum] = 0; 
          if (!state) { 
            if (simNum == 1) { board_1.digitalWrite(12, HIGH); board_3.digitalWrite(6, HIGH); } 
            if (simNum == 2) { board_1.digitalWrite(12, HIGH); } 
            if (simNum == 3) { board_3.digitalWrite(5, HIGH); board_3.digitalWrite(7, HIGH); } 
            if (simNum == 6) { board_1.digitalWrite(12, HIGH); } 
            if (simNum == 14){ board_2.digitalWrite(12, HIGH); } 
            if (simNum == 15){ board_2.digitalWrite(14, HIGH); } 
          }
        }
      } else if (id.startsWith("f")) {
        int f = id.substring(1).toInt();
        fault_active[f] = state; 
        switch(f) {
          case 15: board_1.digitalWrite(0, val_inv); break;
          case 18: board_1.digitalWrite(1, val_inv); break;
          case 21: board_1.digitalWrite(2, val_inv); break;
          case 32: board_1.digitalWrite(3, val_inv); break;
          case 16: board_1.digitalWrite(4, val_inv); break;
          case 19: board_1.digitalWrite(5, val_inv); break;
          case 22: board_1.digitalWrite(6, val_inv); break;
          case 33: board_1.digitalWrite(7, val_inv); break;
          case 17: board_1.digitalWrite(8, val_inv); break;
          case 20: board_1.digitalWrite(9, val_inv); break;
          case 23: board_1.digitalWrite(10, val_inv); break;
          case 34: board_1.digitalWrite(11, val_inv); break;
          case 24: board_1.digitalWrite(12, val_inv); break;
          case 25: board_1.digitalWrite(13, val_inv); break;
          case 28: board_1.digitalWrite(14, val_inv); break;
          case 29: board_1.digitalWrite(15, val_inv); break;
          case 30: board_2.digitalWrite(13, val_inv); break;
          case 31: board_2.digitalWrite(14, val_inv); break;
          case 1: board_3.digitalWrite(0, val_inv); break;
          case 2: board_3.digitalWrite(1, val_inv); break;
          case 3: board_3.digitalWrite(2, val_inv); break;
          case 4: board_3.digitalWrite(3, val_inv); break;
          case 5: board_3.digitalWrite(4, val_inv); break;
          case 6: board_3.digitalWrite(5, val_inv); break;
          case 7: board_3.digitalWrite(6, val_inv); break;
          case 8: board_3.digitalWrite(7, val_inv); break;
          case 10: board_3.digitalWrite(9, val_inv); break;
          case 11: board_3.digitalWrite(10, val_inv); break;
          case 12: board_3.digitalWrite(11, val_inv); break;
          case 13: board_3.digitalWrite(12, val_inv); break;
          case 14: board_3.digitalWrite(13, val_inv); break;
          case 26: board_3.digitalWrite(14, val_inv); break;
          case 27: board_3.digitalWrite(15, val_inv); break;
        }
      } else {
        if (id == "relay_inducer") board_2.digitalWrite(0, val_inv);
        else if (id == "relay_igniter") board_2.digitalWrite(1, val_inv);
        else if (id == "relay_gas_valve") board_2.digitalWrite(2, val_inv);
        else if (id == "relay_heat_blower") board_2.digitalWrite(3, val_inv);
        else if (id == "relay_compressor_motor") board_2.digitalWrite(8, val_inv); 
        else if (id == "relay_compressor_contactor") board_2.digitalWrite(9, val_inv); 
        else if (id == "relay_condenser_y") board_2.digitalWrite(11, val_inv); 
        else if (id == "relay_y_wire_interlock") board_2.digitalWrite(12, val_inv);
        else if (id == "relay_rollout_limit_1") { fault_active[33] = state; board_1.digitalWrite(7, val_inv); }
        else if (id == "relay_rollout_limit_2") { fault_active[34] = state; board_1.digitalWrite(11, val_inv); }
        else if (id == "relay_high_temp_limit") { fault_active[22] = state; board_1.digitalWrite(6, val_inv); }
        else if (id == "relay_flame_sensor") { fault_active[32] = state; board_1.digitalWrite(3, val_inv); }
        else if (id == "relay_condenser_fan") { fault_active[6] = state; board_3.digitalWrite(5, val_inv); }
        else if (id == "relay_low_pressure_switch") { fault_active[7] = state; board_3.digitalWrite(6, val_inv); }
        else if (id == "relay_high_pressure_switch") { fault_active[8] = state; board_3.digitalWrite(7, val_inv); }
        else if (id == "relay_vacuum_pressure_switch") { fault_active[16] = state; board_1.digitalWrite(4, val_inv); }
        else if (id == "relay_pressure_switch_closed") { fault_active[19] = state; board_1.digitalWrite(5, val_inv); }
        else if (id == "relay_inducer_open") { fault_active[15] = state; board_1.digitalWrite(0, val_inv); }
        else if (id == "relay_igniter_open") { fault_active[18] = state; board_1.digitalWrite(1, val_inv); }
        else if (id == "relay_gas_valve_closed") { fault_active[21] = state; board_1.digitalWrite(2, val_inv); }
        else if (id == "relay_gas_valve_open") { fault_active[20] = state; board_1.digitalWrite(9, val_inv); }
        else if (id == "relay_draft_safeguard") { fault_active[17] = state; board_1.digitalWrite(8, val_inv); }
        else if (id == "relay_blocked_flue") { fault_active[23] = state; board_1.digitalWrite(10, val_inv); }
        else if (id == "relay_indoor_fan_off") { fault_active[24] = state; board_1.digitalWrite(12, val_inv); }
        else if (id == "relay_indoor_fan_on") { fault_active[25] = state; board_1.digitalWrite(13, val_inv); }
        else if (id == "relay_shorted_contactor") { fault_active[30] = state; board_2.digitalWrite(13, val_inv); }
        else if (id == "relay_comp_limit_open") { fault_active[31] = state; board_2.digitalWrite(14, val_inv); }
        else if (id == "relay_low_pressure_board_fault") { fault_active[26] = state; board_3.digitalWrite(14, val_inv); }
        else if (id == "relay_high_pressure_board_fault") { fault_active[27] = state; board_3.digitalWrite(15, val_inv); }
        else if (id == "relay_failed_gas_relay") { fault_active[34] = state; board_1.digitalWrite(11, val_inv); }
        else if (id == "relay_a2l_sensor") { fault_active[28] = state; board_1.digitalWrite(14, val_inv); }
        else if (id == "relay_a2l_board_fault") { fault_active[29] = state; board_1.digitalWrite(15, val_inv); }
        else if (id == "relay_failed_gas_valve") { fault_active[21] = state; board_1.digitalWrite(2, val_inv); }
        else if (id == "relay_faulty_high_temp_limit") { fault_active[22] = state; board_1.digitalWrite(6, val_inv); }
        else if (id == "relay_grounded_w_wire") { fault_active[52] = state; }
        else if (id == "relay_shorted_y_to_r") { fault_active[51] = state; }
        else if (id == "relay_shorted_w_to_r") { fault_active[53] = state; }
        else if (id == "relay_draft_inducer_board_fault") { fault_active[17] = state; board_1.digitalWrite(8, val_inv); }
        else if (id == "relay_shorted_ignition_board") { fault_active[18] = state; board_1.digitalWrite(1, val_inv); }
        else if (id == "relay_faulty_a2l_sensor") { fault_active[28] = state; board_1.digitalWrite(14, val_inv); }
        else if (id == "relay_faulty_a2l_board") { fault_active[29] = state; board_1.digitalWrite(15, val_inv); }
        else if (id == "relay_open_contactor_coil") { fault_active[31] = state; board_2.digitalWrite(14, val_inv); }
        else if (id == "relay_shorted_contactor_coil") { fault_active[54] = state; }
      }
      
      force_telemetry_update = true;
      request->send(200, "text/plain", "OK");
    } else { request->send(400, "text/plain", "Bad Request"); }
  });

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request){ 
    request->send(200, "application/json", getStatusJSON()); 
  });
  
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "*");

  server.begin();
}

uint32_t heartbeat_timer = 0;
uint32_t wifi_reconnect_timer = 0;
void handle_system_health() {
  if (ota_in_progress) return; // Allow OTA/Update handlers to control the LED
  uint32_t now = millis();
  if (now >= led_timer) {
    led_timer = now + 1000; 
    if (is_ap_mode) { status_led.setPixelColor(0, status_led.Color(150, 0, 150)); } 
    else if (WiFi.status() == WL_CONNECTED) { status_led.setPixelColor(0, status_led.Color(0, 255, 0)); } 
    else { status_led.setPixelColor(0, status_led.Color(255, 0, 0)); }
    status_led.show();
  }
  if (now >= heartbeat_timer) {
    heartbeat_timer = now + 500;
    force_telemetry_update = true;
  }
}

float add_noise(float base, float variance) {
  float r = (esp_random() % 1000) / 1000.0; 
  return base + ((r * (variance * 2.0)) - variance);
}

// ==========================================
// 🚀 DYNAMIC PHYSICS & FAULT ENGINE
// ==========================================
uint32_t telemetry_timer = 0;
void handle_telemetry() {
  uint32_t now = millis();
  uint32_t current_interval = (last_comp_state && (now - comp_start_time < 2000)) ? 100 : 500;
  if (now < telemetry_timer) return;
  telemetry_timer = now + current_interval; 

  float ref_mult = 1.0; 
  if (current_refrigerant == "R22") ref_mult = 0.60;
  else if (current_refrigerant == "R32") ref_mult = 1.04;
  else if (current_refrigerant == "R454B") ref_mult = 0.98;
  else if (current_refrigerant == "R134a") ref_mult = 0.40;
  else if (current_refrigerant == "R404A") ref_mult = 0.75;
  else if (current_refrigerant == "R407C") ref_mult = 0.65;

  float target_eq_press = (145.0 + ((set_od_temp - 70.0) * 1.5)) * ref_mult; 

  // --- PRESSURE SWITCH THRESHOLDS ---
  float lps_trip = 40.0; float lps_reset = 80.0;
  float hps_trip = 610.0; float hps_reset = 475.0; 

  if (current_refrigerant == "R410A") { lps_trip = 40.0; lps_reset = 80.0; hps_trip = 610.0; hps_reset = 475.0; } 
  else if (current_refrigerant == "R454B") { lps_trip = 35.0; lps_reset = 70.0; hps_trip = 575.0; hps_reset = 450.0; } 
  else if (current_refrigerant == "R32") { lps_trip = 45.0; lps_reset = 85.0; hps_trip = 640.0; hps_reset = 500.0; } 
  else if (current_refrigerant == "R22") { lps_trip = 25.0; lps_reset = 60.0; hps_trip = 400.0; hps_reset = 300.0; } 
  else if (current_refrigerant == "R407C") { lps_trip = 25.0; lps_reset = 55.0; hps_trip = 420.0; hps_reset = 320.0; } 
  else if (current_refrigerant == "R134a") { lps_trip = 10.0; lps_reset = 25.0; hps_trip = 300.0; hps_reset = 200.0; } 
  else if (current_refrigerant == "R404A") { lps_trip = 15.0; lps_reset = 35.0; hps_trip = 450.0; hps_reset = 350.0; }

  if (sim_od_low_press <= lps_trip) phys_lps_tripped = true;
  else if (sim_od_low_press >= lps_reset) phys_lps_tripped = false;
  if (sim_od_high_press >= hps_trip) phys_hps_tripped = true;
  else if (sim_od_high_press <= hps_reset) phys_hps_tripped = false;

  bool lps_open = phys_lps_tripped || fault_active[7] || fault_active[26]; 
  bool hps_open = phys_hps_tripped || fault_active[8] || fault_active[27];
  
  bool y_broken = fault_active[2] || sim_active[14];
  bool y_circuit_intact = (state_y && !lps_open && !hps_open && !y_broken);
  bool contactor_pulled = y_circuit_intact || fault_active[30]; 
  bool is_compressor = contactor_pulled && !fault_active[31] && !sim_active[15]; 

  bool phys_cooling = is_compressor;
  bool phys_heating = state_w; // In a furnace, compressor does not run in heating

  bool id_fan_fail = fault_active[24] || sim_active[1] || sim_active[6];
  bool od_fan_fail = fault_active[6] || sim_active[3]; 

  // --- FAULT MAPPING REDECLARATIONS ---
  bool fault_non_condensables = fault_active[40];
  bool fault_stuck_id_txv     = fault_active[41];
  bool fault_clogged_txv      = fault_active[42];
  bool fault_clogged_piston   = fault_active[43];
  bool fault_comp_bypass      = fault_active[44];
  bool fault_inefficient_comp = fault_active[45];
  bool fault_low_id_cfm       = fault_active[46];
  bool fault_high_id_cfm      = fault_active[47];

  float target_supply = set_id_temp; 
  float target_low = sim_od_low_press;
  float target_high = sim_od_high_press;
  float target_sh = 12.0;

  float rh_variance = (set_rh - 50.0) / 10.0; 
  float latent_heat_penalty = (rh_variance > 0) ? (rh_variance * 1.5) : (rh_variance * 1.0);
  float latent_pressure_penalty = (rh_variance > 0) ? (rh_variance * 3.0) : (rh_variance * 1.5);

  float line_friction_delta = 0.0;

  if (!is_compressor) {
    target_low = target_eq_press; target_high = target_eq_press;
    float bleed_rate = id_is_txv ? 0.005f : 0.025f; 
    
    sim_od_high_press += (target_eq_press - sim_od_high_press) * bleed_rate;
    sim_od_low_press += (target_eq_press - sim_od_low_press) * bleed_rate;
    
    sim_od_suction_temp += (set_od_temp - sim_od_suction_temp) * 0.05f;
    sim_od_liquid_temp += (set_od_temp - sim_od_liquid_temp) * 0.05f;
    sim_od_discharge += (set_od_temp - sim_od_discharge) * 0.05f;
    target_supply = set_id_temp + (phys_heating ? 65.0f : 0.0f); // 65F temp rise for gas furnace
    
  } else if (phys_cooling) {
    target_high = ((set_od_temp * 3.5f) + 50.0f) * ref_mult; 
    float txv_shift = id_is_txv ? 1.0f : 0.0f;
    target_low = ((set_id_temp * 2.0f) - 30.0f + (latent_pressure_penalty * txv_shift)) * ref_mult;   
    target_sh = id_is_txv ? 12.0f : ((set_id_temp - set_od_temp + 30.0f) + (latent_heat_penalty * 2.0f));

    line_friction_delta = od_fan_fail ? 8.0f : 18.0f; 

    if (fault_non_condensables) { target_high += 130.0f * ref_mult; target_low += 5.0f * ref_mult; line_friction_delta += 10.0f; }
    if (fault_stuck_id_txv) { target_low += 25.0f * ref_mult; target_high -= 30.0f * ref_mult; target_sh = 0.5f; }
    if (fault_clogged_txv || fault_clogged_piston) { target_low -= 35.0f * ref_mult; target_high -= 15.0f * ref_mult; target_sh = 35.0f; }
    if (fault_comp_bypass) { target_low += 40.0f * ref_mult; target_high -= 75.0f * ref_mult; }
    if (fault_inefficient_comp) { target_low += 25.0f * ref_mult; target_high -= 45.0f * ref_mult; }
    if (fault_low_id_cfm) { target_low -= 20.0f * ref_mult; target_sh = 2.0f; target_supply -= 12.0f; line_friction_delta -= 6.0f; }
    if (fault_high_id_cfm) { target_low += 15.0f * ref_mult; target_sh += 15.0f; target_supply += 8.0f; line_friction_delta += 5.0f; }

    float low_abs = sim_od_low_press + 14.7f;
    if (low_abs < 1.0f) low_abs = 1.0f;
    float comp_ratio = (sim_od_high_press + 14.7f) / low_abs;
    float vol_eff_penalty = (comp_ratio - 2.5f) * 4.0f; 
    if (vol_eff_penalty < 0.0f) vol_eff_penalty = 0.0f;

    target_low += vol_eff_penalty * ref_mult; 
    
    if (od_fan_fail) target_high = 600.0f * ref_mult; 
    if (id_fan_fail) { target_low = 20.0f * ref_mult; line_friction_delta = 2.0f; }   

    float estimated_low_sat = (target_low / ref_mult) * 0.3f + 10.0f; 
    sim_od_suction_temp = add_noise(estimated_low_sat + target_sh, 0.4f);  
    sim_od_liquid_temp = add_noise(set_od_temp + 10.0f, 0.4f);  
    sim_od_discharge = add_noise(sim_od_suction_temp + (comp_ratio * 20.0f) + 45.0f + vol_eff_penalty, 2.0f);
    target_supply = (set_id_temp - 20.0f) + latent_heat_penalty; 

    if (fault_comp_bypass) sim_od_suction_temp += 35.0f; 
    if (fault_non_condensables) sim_od_liquid_temp -= 12.0f; 
    if (fault_clogged_txv || fault_clogged_piston) sim_od_liquid_temp -= 15.0f; 

    if (od_fan_fail) sim_od_discharge = add_noise(220.0f, 5.0f);
    if (id_fan_fail) sim_od_suction_temp = add_noise(25.0f, 0.5f); 

    if (phys_heating) target_supply += 65.0f; // Ensure heat is simulated if both run simultaneously
  }

  if (force_pressure_snap && is_compressor) {
      sim_od_low_press = target_low;
      sim_od_high_press = target_high;
      sim_od_liquid_press = target_high - (line_friction_delta * 1.8f * ref_mult); 
      force_pressure_snap = false; 
  } else if (is_compressor) {
      // Discharge pressure (high_press) reacts quickly to compressor strokes
      sim_od_low_press += (target_low - sim_od_low_press) * 0.08f;
      sim_od_high_press += (target_high - sim_od_high_press) * 0.12f; 
      
      // Liquid line pressure is damped/delayed by the condenser volume
      float true_liquid_target = target_high - (line_friction_delta * 1.8f * ref_mult);
      sim_od_liquid_press += (true_liquid_target - sim_od_liquid_press) * 0.035f; 
  } else {
      sim_od_liquid_press += (target_high - sim_od_liquid_press) * 0.02f; // Equalize back to static
  }

  if (id_fan_fail) { if (phys_heating) target_supply = 160.0f; else target_supply = set_id_temp; }
  sim_id_supply_temp += (target_supply - sim_id_supply_temp) * 0.05f;
  sim_od_ambient = add_noise(set_od_temp, 0.2f);
  sim_id_return_temp = add_noise(set_id_temp, 0.2f);
  sim_id_ambient = add_noise(set_id_temp, 0.2f); 
  sim_id_rh = add_noise(set_rh, 0.5f);

  if (is_compressor && !last_comp_state) {
      comp_start_time = now;
  }
  last_comp_state = is_compressor;

  if (is_compressor) {
      // Locked Rotor Amps simulation
      if (fault_active[50]) {
          sim_comp_amps = 143.0f; 
      } else if (now - comp_start_time < 400) {
          sim_comp_amps = 143.0f; 
      } else {
          sim_comp_amps = 10.0f + ((sim_od_high_press / ref_mult) * 0.035f);
          if (fault_comp_bypass) sim_comp_amps -= 6.5f; 
          if (fault_inefficient_comp) sim_comp_amps -= 4.0f;
          sim_comp_amps = add_noise(sim_comp_amps, 0.2f);
      }
  } else {
      sim_comp_amps = 0.0f;
  }

  if (is_compressor && !od_fan_fail) {
      sim_od_fan_amps = add_noise(1.8f, 0.05f);
  } else {
      sim_od_fan_amps = 0.0f;
  }

  bool id_fan_on = (state_g || state_y || heat_blower_on || fault_active[25]) && !id_fan_fail;
  if (id_fan_on) {
      float base_id_amps = 4.5f; 
      if (fault_low_id_cfm) base_id_amps = 3.6f; 
      if (fault_high_id_cfm) base_id_amps = 5.3f;
      sim_id_fan_amps = add_noise(base_id_amps, 0.08f);
  } else {
      sim_id_fan_amps = 0.0f;
  }
}

void handle_hvac_logic() {
  uint32_t now = millis();
  bool current_w = readDebounced(PIN_W, state_w, debounce_w);
  bool current_y = readDebounced(PIN_Y, state_y, debounce_y);
  bool current_g = readDebounced(PIN_G, state_g, debounce_g);

  bool send_update = false;
  if (current_w != last_w_state || current_y != last_y_state || current_g != last_g_state) { send_update = true; }

  board_3.digitalWrite(6, (phys_lps_tripped || fault_active[7]) ? LOW : HIGH); 
  board_3.digitalWrite(7, (phys_hps_tripped || fault_active[8]) ? LOW : HIGH); 

  if (current_w && furnace_state == FURNACE_IDLE) {
    furnace_state = FURNACE_PRE_PURGE;
    furnace_timer = now;
    ignition_retry_count = 0;
    set_furnace_relays(true, false, false, false);
    send_update = true;
  }
  
  switch (furnace_state) {
    case FURNACE_IDLE:
      if (blower_off_delay > 0 && now >= blower_off_delay) {
        set_furnace_relays(false, false, false, false);
        blower_off_delay = 0;
        send_update = true;
      }
      break;
    case FURNACE_PRE_PURGE:
      if (!current_w) {
        furnace_state = FURNACE_POST_PURGE;
        furnace_timer = now;
        send_update = true;
      } else if (now - furnace_timer >= 15000) {
        // Check for safeties before proceeding
        bool pressure_switch_closed = !fault_active[16] && !fault_active[23]; // Not stuck open, flue not blocked
        bool limit_ok = !fault_active[22] && !fault_active[33]; // Primary and rollout limits are closed
        
        if (pressure_switch_closed && limit_ok) {
          furnace_state = FURNACE_IGNITER_WARMUP;
          furnace_timer = now;
          set_furnace_relays(true, true, false, false);
          send_update = true;
        } else {
          // Safety check failed, go to lockout
          furnace_state = FURNACE_LOCKOUT;
          send_update = true;
        }
      }
      break;
    case FURNACE_IGNITER_WARMUP:
      if (!current_w) {
        furnace_state = FURNACE_POST_PURGE;
        furnace_timer = now;
        set_furnace_relays(true, false, false, false);
        send_update = true;
      } else if (now - furnace_timer >= 10000) {
        furnace_state = FURNACE_TRIAL_FOR_IGNITION;
        furnace_timer = now;
        set_furnace_relays(true, true, true, false);
        send_update = true;
      }
      break;
    case FURNACE_TRIAL_FOR_IGNITION:
      if (!current_w) {
        furnace_state = FURNACE_POST_PURGE;
        furnace_timer = now;
        set_furnace_relays(true, false, false, false);
        send_update = true;
      } else if (now - furnace_timer >= 4000) { // Flame proving window
        bool flame_sensed = !fault_active[32]; // Flame sensor is not dirty/failed
        if (flame_sensed) {
          furnace_state = FURNACE_HEATING;
          set_furnace_relays(true, false, true, false); 
          blower_on_delay = now + 30000; 
          send_update = true;
        } else {
          // Trial for ignition failed
          ignition_retry_count++;
          set_furnace_relays(true, false, false, false); // Turn off gas and igniter
          furnace_state = (ignition_retry_count >= 3) ? FURNACE_LOCKOUT : FURNACE_PRE_PURGE; // Retry or lockout
          furnace_timer = now;
          send_update = true;
        }
      }
      break;
    case FURNACE_HEATING:
      if (!current_w) {
        furnace_state = FURNACE_POST_PURGE;
        furnace_timer = now;
        set_furnace_relays(true, false, false, heat_blower_on);
        send_update = true;
      } else if (sim_id_supply_temp > 150.0f) {
        // High Limit Switch Tripped during run
        furnace_state = FURNACE_LOCKOUT;
        set_furnace_relays(true, false, false, true); // Keep inducer and blower on to cool down
        limit_trip_count++;
        send_update = true;
      } else if (blower_on_delay > 0 && now >= blower_on_delay) {
        set_furnace_relays(true, false, true, true);
        blower_on_delay = 0;
        send_update = true;
      }
      break;
    case FURNACE_POST_PURGE:
      if (now - furnace_timer >= 15000) { 
        furnace_state = FURNACE_IDLE;
        set_furnace_relays(false, false, false, heat_blower_on);
        blower_off_delay = now + 90000;
        send_update = true;
      }
      break;
    case FURNACE_LOCKOUT:
      if (!current_w) {
        furnace_state = FURNACE_IDLE;
        set_furnace_relays(false, false, false, false);
        send_update = true;
      } else if (sim_id_supply_temp < 110.0f) {
        // Auto-reset from high-limit trip when cooled down
        furnace_state = FURNACE_PRE_PURGE; // Restart sequence
        furnace_timer = now;
        set_furnace_relays(true, false, false, true); // Keep blower on
        send_update = true;
      }
      break;
  }

  if (current_y && !last_y_state) { if (!sim_active[14]) { board_2.digitalWrite(12, LOW); board_2.digitalWrite(11, LOW); board_2.digitalWrite(9, LOW); if (!sim_active[15]) board_2.digitalWrite(8, LOW); } }
  if (!current_y && last_y_state) { if (!sim_active[14]) { board_2.digitalWrite(12, HIGH); board_2.digitalWrite(11, HIGH); } board_2.digitalWrite(9, HIGH); board_2.digitalWrite(8, HIGH); }
  
  last_w_state = current_w; last_y_state = current_y; last_g_state = current_g;
  if (send_update) { notifyClients(); }
}

void handle_simulations() {
  uint32_t now = millis();
  bool current_w = state_w; bool current_y = state_y;

  if (sim_active[1] && current_y) {
    if (sim_step[1] == 0) { board_1.digitalWrite(12, LOW); sim_timer[1] = now + 25000; sim_step[1] = 1; } 
    else if (sim_step[1] == 1 && now >= sim_timer[1]) { board_3.digitalWrite(6, LOW); sim_step[1] = 2; }
  } else if (sim_active[1] && !current_y) { board_1.digitalWrite(12, HIGH); board_3.digitalWrite(6, HIGH); sim_step[1] = 0; }
  
  if (sim_active[2] && current_w) {
    if (sim_step[2] == 0) { board_1.digitalWrite(12, LOW); sim_timer[2] = now + 20000; sim_step[2] = 1; } 
    else if (sim_step[2] == 1 && now >= sim_timer[2]) { sim_step[2] = 2; }
  } else if (sim_active[2] && !current_w) { board_1.digitalWrite(12, HIGH); sim_step[2] = 0; }
  
  if (sim_active[3] && current_y) {
    if (sim_step[3] == 0) { board_3.digitalWrite(5, LOW); sim_timer[3] = now + 30000; sim_step[3] = 1; } 
    else if (sim_step[3] == 1 && now >= sim_timer[3]) { board_3.digitalWrite(7, LOW); sim_step[3] = 2; }
  } else if (sim_active[3] && !current_y) { board_3.digitalWrite(5, HIGH); board_3.digitalWrite(7, HIGH); sim_step[3] = 0; }
  
  if (sim_active[6] && (current_y || current_w)) {
    if (now >= sim_timer[6]) {
      if (sim_step[6] == 0) { board_1.digitalWrite(12, LOW); sim_timer[6] = now + 5000; sim_step[6] = 1; } 
      else { board_1.digitalWrite(12, HIGH); sim_timer[6] = now + 15000; sim_step[6] = 0; }
    }
  } else if (sim_active[6]) { board_1.digitalWrite(12, HIGH); sim_step[6] = 0; sim_timer[6] = 0; }
  
  if (sim_active[5] && furnace_state == FURNACE_HEATING) { // Intermittent Flame Sensor
    if (now >= sim_timer[5]) {
      if (sim_step[5] == 0) { fault_active[32] = true; board_1.digitalWrite(3, LOW); sim_timer[5] = now + 2000; sim_step[5] = 1; } // Drop flame for 2s
      else { fault_active[32] = false; board_1.digitalWrite(3, HIGH); sim_timer[5] = now + 25000; sim_step[5] = 0; } // Flame good for 25s
    }
  } else if (sim_active[5]) { fault_active[32] = false; board_1.digitalWrite(3, HIGH); sim_step[5] = 0; sim_timer[5] = 0; }

  if (sim_active[8] && furnace_state == FURNACE_PRE_PURGE) { // Fluttering Pressure Switch
    if (now >= sim_timer[8]) {
      if (sim_step[8] % 2 == 0) { fault_active[16] = true; board_1.digitalWrite(4, LOW); }
      else { fault_active[16] = false; board_1.digitalWrite(4, HIGH); }
      sim_timer[8] = now + (esp_random() % 800 + 200);
      sim_step[8]++;
    }
  } else if (sim_active[8]) { fault_active[16] = false; board_1.digitalWrite(4, HIGH); sim_step[8] = 0; sim_timer[8] = 0; }

  if (sim_active[9] && furnace_state == FURNACE_POST_PURGE) { // Leaky Gas Valve
    if (sim_step[9] == 0) { fault_active[20] = true; board_1.digitalWrite(9, LOW); sim_step[9] = 1; }
  } else if (sim_active[9]) { fault_active[20] = false; board_1.digitalWrite(9, HIGH); sim_step[9] = 0; }
  
  
  if (sim_active[7]) { if (current_y) { board_3.digitalWrite(7, LOW); } }
  
  if (sim_active[14] && current_y) {
    if (now >= sim_timer[14]) {
      if (sim_step[14] == 0) { board_2.digitalWrite(12, LOW); sim_timer[14] = now + (esp_random() % 2000 + 500); sim_step[14] = 1; } 
      else { board_2.digitalWrite(12, HIGH); sim_timer[14] = now + (esp_random() % 10000 + 5000); sim_step[14] = 0; }
    }
  } else if (sim_active[14] && !current_y) { board_2.digitalWrite(12, HIGH); sim_step[14] = 0; sim_timer[14] = 0; }
  
  if (sim_active[15] && current_y) {
    if (sim_step[15] == 0) { sim_timer[15] = now + 45000; sim_step[15] = 1; } 
    else if (sim_step[15] == 1 && now >= sim_timer[15]) { board_2.digitalWrite(14, LOW); sim_timer[15] = now + 60000; sim_step[15] = 2; } 
    else if (sim_step[15] == 2 && now >= sim_timer[15]) { board_2.digitalWrite(14, HIGH); sim_timer[15] = now + 45000; sim_step[15] = 1; }
  } else if (sim_active[15] && !current_y) { board_2.digitalWrite(14, HIGH); sim_step[15] = 0; sim_timer[15] = 0; }
}

void loop() {
  if (is_ap_mode) { dnsServer.processNextRequest(); } 
  else if (wifi_ssid.length() > 0 && WiFi.status() != WL_CONNECTED) {
    uint32_t now = millis();
    if (now - wifi_reconnect_timer >= 10000) {
      wifi_reconnect_timer = now;
      Serial.println("WiFi disconnected. Attempting reconnect...");
      WiFi.reconnect();
      if (WiFi.status() != WL_CONNECTED) {
        WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
      }
    }
  }
  ArduinoOTA.handle();
  ws.cleanupClients(); 
  
  if (pending_reboot && millis() > reboot_timer) {
    ESP.restart();
  }

  // =============================================================
  // TELEMETRY & EVENT DISPATCH ENGINE
  // =============================================================
  if (force_telemetry_update) { notifyClients(true); force_telemetry_update = false; }

  if (deviceConnected && !oldDeviceConnected) {
    Serial.println("Smartphone Connection Verified via BLE.");
    oldDeviceConnected = deviceConnected;
    ble_restart_state = BLE_IDLE; // A device connected, so we can stop any restart process
  } else if (!deviceConnected && oldDeviceConnected) {
    Serial.println("BLE device disconnected. Starting non-blocking restart of advertising...");
    oldDeviceConnected = false; // Only trigger this once
    ble_restart_state = BLE_DISCONNECTED;
    ble_restart_timer = millis();
  }

  // Non-blocking BLE advertising restart state machine
  if (ble_restart_state == BLE_DISCONNECTED && millis() - ble_restart_timer > 200) {
    NimBLEDevice::getAdvertising()->stop();
    Serial.println("BLE Advertising stopped.");
    ble_restart_state = BLE_RESTARTING;
    ble_restart_timer = millis();
  }
  
  if (ble_restart_state == BLE_RESTARTING && millis() - ble_restart_timer > 50) {
    NimBLEDevice::getAdvertising()->start();
    Serial.println("BLE Advertising restarted.");
    ble_restart_state = BLE_IDLE; // Finished
  }

  handle_hvac_logic();
  handle_telemetry();
  handle_simulations();    
  sendEngineHeartbeat();
  handle_system_health();

}