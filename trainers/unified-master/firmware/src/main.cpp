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
#include "main.h"
#include <AsyncMqttClient.h>
#include "FurnaceController.h"
#include "PhysicsEngine.h"
#include "TrainerInstance.h"

// ==========================================
// DOCKER ENGINE LINK
// ==========================================
const char* ENGINE_CONFIG_FILE = "/engine.json";
String engine_base_url = "";
String EDGE_ID_HEAT_PUMP = "";
String EDGE_LABEL_HEAT_PUMP = "";
const char* TRAINER_TYPE_HEAT_PUMP = "heat_pump";
String EDGE_ID_FURNACE = "";
String EDGE_LABEL_FURNACE = "";
const char* TRAINER_TYPE_FURNACE = "straight_ac_furnace";
String EDGE_ID = "";
String EDGE_LABEL = "";
const char* TRAINER_TYPE = TRAINER_TYPE_HEAT_PUMP;
const uint16_t TRAINER_INSTANCE_ID = TRAINER_INSTANCE;
bool is_identity_resolved = false;
String OTA_HOSTNAME = "";
const uint32_t ENGINE_HEARTBEAT_INTERVAL_MS = 200;
const uint32_t ENGINE_DISCOVERY_RETRY_MS = 15000;
uint32_t engine_heartbeat_timer = 0;
uint32_t next_engine_discovery_ms = 0;
bool engine_link_enabled = true;

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
const int PIN_O = 5;
const int PIN_G = 4;
const int PIN_IDENTITY = 14;
const int PIN_BLOWER_MONITOR = 10;
const int PIN_GAS_VALVE_MONITOR = 11;

enum TrainerType {
  HEAT_PUMP,
  STRAIGHT_AC_FURNACE
};

TrainerType active_trainer_type = HEAT_PUMP;
bool identity_pin_boot_is_low = false;

const uint16_t STATUS_LED_PIXEL_COUNT = 4;
const uint16_t STATUS_LED_PRIMARY_INDEX = 0;
const uint16_t STATUS_LED_OTA_MARKER_INDEX = 3;
Adafruit_NeoPixel status_led(STATUS_LED_PIXEL_COUNT, 48, NEO_GRB + NEO_KHZ800);
uint32_t led_timer = 0;
bool ota_in_progress = false; 

enum LedControlMode {
  LED_MODE_STATUS,
  LED_MODE_TEST,
  LED_MODE_MANUAL
};

LedControlMode led_control_mode = LED_MODE_STATUS;
uint32_t led_manual_colors[STATUS_LED_PIXEL_COUNT] = {0, 0, 0, 0};
uint16_t led_test_index = 0;
uint8_t led_test_color_step = 0;
uint32_t led_test_interval_ms = 450;

DNSServer dnsServer;
bool is_ap_mode = false; 
bool setup_ap_active = false;

// ==========================================
// GLOBAL TIMERS & STATES
// ==========================================
uint32_t on_hs1_t1 = 0, off_hs1_t1 = 0; bool hs1_t1_on = false;
uint32_t on_hs1_t23 = 0, off_hs1_t23 = 0; bool hs1_t23_on = false;
uint32_t on_hs2_t1 = 0, off_hs2_t1 = 0; bool hs2_t1_on = false;
uint32_t on_hs2_t23 = 0, off_hs2_t23 = 0; bool hs2_t23_on = false;
uint32_t on_hs3 = 0, off_hs3 = 0; bool hs3_on = false;
bool seq3_timer_started = false;

bool last_w_state = false; bool last_y_state = false; bool last_o_state = false; bool last_g_state = false;

uint32_t debounce_w = 0, debounce_y = 0, debounce_o = 0, debounce_g = 0;
bool state_w = false, state_y = false, state_o = false, state_g = false;

// --- DYNAMIC ELECTRICAL TELEMETRY ---
float sim_comp_amps = 0.0;
float sim_od_fan_amps = 0.0;
float sim_id_fan_amps = 0.0;
float sim_hs_amps = 0.0;
uint32_t comp_start_time = 0;
bool last_comp_state = false;

bool is_defrosting = false; 
bool force_defrost = false; 

bool phys_lps_tripped = false;
bool phys_hps_tripped = false;

bool sim_active[16] = {false};
bool fault_active[55] = {false}; 

uint32_t sim_timer[16] = {0};   
int sim_step[16] = {0};         
int limit_trip_count = 0;       
uint32_t heat_runtime_start = 0;
int8_t override_defrost_sensor = -1; 
String latest_diagnosis = "None";

// --- DYNAMIC REFRIGERANT PHYSICS ---
String current_refrigerant = "R410A"; 
bool force_pressure_snap = false;     
bool id_is_txv = true;
bool od_is_txv = true;
bool is_b_type = false;

// --- DYNAMIC AMBIENT SLIDERS ---
float set_od_temp = 90.0;
float set_id_temp = 75.0;
float set_rh = 50.0; 

float sim_od_low_press = 145.0;  
float sim_od_high_press = 145.0; 
float sim_od_liquid_press = 145.0; // Fixed: Added raw float tracking variable for liquid gauge data stream
float sim_od_suction_temp = 90.0;
float sim_od_liquid_temp = 90.0;
float sim_od_ambient = 90.0;
float sim_od_discharge = 90.0;

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
String wifi_ssid = "ComfortSC";
String wifi_pass = "8037945526";
String authToken = "";
String authRole = "";
uint32_t authExpiry = 0;
const uint32_t AUTH_TOKEN_TTL = 28800;
uint32_t wifi_reconnect_timer = 0;
bool wifi_was_connected = false;
uint32_t wifi_disconnected_since = 0;
uint32_t wifi_connected_flash_until = 0;
const uint32_t WIFI_CONNECTING_GRACE_MS = 15000;
const uint32_t WIFI_CONNECTED_FLASH_MS = 10000;
const uint32_t WIFI_AP_FAILOVER_MS = 60000;
bool pending_reboot = false;
uint32_t reboot_timer = 0;
bool i2c_boards_present = false;
AsyncMqttClient mqttClient;
TimerHandle_t mqttReconnectTimer;

FurnaceController furnace_controller(board_2, i2c_boards_present);
PhysicsEngine furnace_physics;

bool hb_last_w = false;
bool hb_last_y = false;
bool hb_last_o = false;
bool hb_last_g = false;
bool hb_last_lps = false;
bool hb_last_hps = false;
TaskHandle_t comm_task_handle = nullptr;
TaskHandle_t control_task_handle = nullptr;
bool dual_core_runtime_enabled = false;

const uint32_t COMM_TASK_SLICE_MS = 5;
const uint32_t CONTROL_TASK_SLICE_MS = 10;

void reset_all_faults_and_sims();
void handleDiagnosis(String submitted);
void detectTrainerTypeAtBoot();
void applyTrainerIdentityMetadata();
void runHeatPumpControlSlice();
void runFurnaceControlSlice();
void syncFurnaceTelemetryToUnifiedState();
void runCommsSlice();
void runControlSlice();
void commTask(void* parameter);
void controlTask(void* parameter);
uint32_t getModeConnectedLedColor();
void connectToMqtt();
void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total);
void setStatusLeds(uint32_t primaryColor, bool otaMarkerBlue = false, uint8_t otaBlueLevel = 255);
const char* getLedModeName();
uint8_t clampColorByte(int value);
void applyManualLeds();
void stepLedTestPattern();

bool readOptoActiveLow(int pin) {
  return digitalRead(pin) == LOW;
}

uint32_t getModeConnectedLedColor() {
  // Connected Wi-Fi indication is always green for clear field diagnostics.
  return status_led.Color(0, 255, 0);
}

void setStatusLeds(uint32_t primaryColor, bool otaMarkerBlue, uint8_t otaBlueLevel) {
  status_led.setPixelColor(STATUS_LED_PRIMARY_INDEX, primaryColor);
  for (uint16_t i = 1; i < status_led.numPixels(); i++) {
    if (otaMarkerBlue && i == STATUS_LED_OTA_MARKER_INDEX) {
      status_led.setPixelColor(i, status_led.Color(0, 0, otaBlueLevel));
    } else {
      status_led.setPixelColor(i, 0);
    }
  }
  status_led.show();
}

const char* getLedModeName() {
  switch (led_control_mode) {
    case LED_MODE_TEST: return "test";
    case LED_MODE_MANUAL: return "manual";
    default: return "status";
  }
}

uint8_t clampColorByte(int value) {
  if (value < 0) return 0;
  if (value > 255) return 255;
  return static_cast<uint8_t>(value);
}

void applyManualLeds() {
  for (uint16_t i = 0; i < status_led.numPixels(); i++) {
    status_led.setPixelColor(i, led_manual_colors[i]);
  }
  status_led.show();
}

void stepLedTestPattern() {
  uint32_t testColor;
  switch (led_test_color_step) {
    case 0: testColor = status_led.Color(255, 0, 0); break;
    case 1: testColor = status_led.Color(0, 255, 0); break;
    case 2: testColor = status_led.Color(0, 0, 255); break;
    default: testColor = status_led.Color(255, 255, 255); break;
  }

  for (uint16_t i = 0; i < status_led.numPixels(); i++) {
    status_led.setPixelColor(i, (i == led_test_index) ? testColor : 0);
  }
  status_led.show();

  led_test_index++;
  if (led_test_index >= status_led.numPixels()) {
    led_test_index = 0;
    led_test_color_step = (led_test_color_step + 1) % 4;
  }
}

String formatTrainerInstance() {
  char instance[8];
  snprintf(instance, sizeof(instance), "%02u", TRAINER_INSTANCE_ID);
  return String(instance);
}

void detectTrainerTypeAtBoot() {
  pinMode(PIN_IDENTITY, INPUT_PULLUP);
  delay(10);
  identity_pin_boot_is_low = (digitalRead(PIN_IDENTITY) == LOW);
  active_trainer_type = identity_pin_boot_is_low ? STRAIGHT_AC_FURNACE : HEAT_PUMP;
}

void applyTrainerIdentityMetadata() {
  String instance = formatTrainerInstance(); // e.g., "01"

  // Simplified, consistent naming scheme based on the embedded trainer number.
  // This is used for BLE name, mDNS hostname, and backend identification.
  EDGE_ID = "trainer" + instance;
  EDGE_LABEL = "Trainer " + instance;
  OTA_HOSTNAME = "trainer" + instance;

  // Set the trainer type for control logic, but it no longer affects the name.
  if (active_trainer_type == STRAIGHT_AC_FURNACE) {
    TRAINER_TYPE = TRAINER_TYPE_FURNACE;
  } else {
    TRAINER_TYPE = TRAINER_TYPE_HEAT_PUMP;
  }

  // With a hardcoded trainer number, the identity is always considered resolved at boot.
  is_identity_resolved = true;
}

bool probePcf8575(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

void detectI2CBoards() {
  bool b1 = probePcf8575(0x20);
  bool b2 = probePcf8575(0x21);
  bool b3 = probePcf8575(0x22);
  i2c_boards_present = b1 && b2 && b3;
  if (i2c_boards_present) {
    Serial.println("I2C relay boards detected.");
  } else {
    Serial.println("I2C relay boards not detected. Running in no-relay mode.");
  }
}

String loadEngineBaseUrl() {
  if (!LittleFS.exists(ENGINE_CONFIG_FILE)) return String();
  File f = LittleFS.open(ENGINE_CONFIG_FILE, FILE_READ);
  if (!f) return String();
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, f);
  f.close();
  if (error) return String();
  return doc["base_url"].as<String>();
}

void saveEngineBaseUrl(const String& baseUrl) {
  if (baseUrl.length() == 0) return;
  File f = LittleFS.open(ENGINE_CONFIG_FILE, FILE_WRITE);
  if (!f) return;
  JsonDocument doc;
  doc["base_url"] = baseUrl;
  serializeJson(doc, f);
  f.close();
}

String discoverEngineBaseUrl(bool allowStored = true) {
  String stored = loadEngineBaseUrl();
  if (allowStored && stored.length() > 0) return stored;

  const char* hostCandidates[] = {"trainer-engine", "trainer-engine.local", "vesta-engine", "vesta-engine.local", "hvac-engine", "hvac-engine.local"};
  for (const char* host : hostCandidates) {
    IPAddress resolved = MDNS.queryHost(host);
    if (resolved != IPAddress(0, 0, 0, 0)) {
      return "http://" + resolved.toString() + ":8000";
    }
  }

  // Scan the /24 subnet for a host responding on :8000/api/edges
  IPAddress localIP = WiFi.localIP();
  if (localIP != IPAddress(0, 0, 0, 0)) {
    HTTPClient scanHttp;
    char scanBuf[42];
    for (int scanI = 1; scanI < 255; scanI++) {
      if (scanI == localIP[3]) continue;
      snprintf(scanBuf, sizeof(scanBuf), "http://%d.%d.%d.%d:8000/api/edges",
               localIP[0], localIP[1], localIP[2], scanI);
      scanHttp.begin(scanBuf);
      scanHttp.setTimeout(300);
      int scanCode = scanHttp.GET();
      scanHttp.end();
      if (scanCode == 200) {
        char scanBase[32];
        snprintf(scanBase, sizeof(scanBase), "http://%d.%d.%d.%d:8000",
                 localIP[0], localIP[1], localIP[2], scanI);
        Serial.printf("Engine found via scan: %s\\n", scanBase);
        return String(scanBase);
      }
    }
    Serial.println("Engine not found on subnet.");
  }

  IPAddress gateway = WiFi.gatewayIP();
  if (gateway != IPAddress(0, 0, 0, 0)) {
    return "http://" + gateway.toString() + ":8000";
  }

  IPAddress dns = WiFi.dnsIP(0);
  if (dns != IPAddress(0, 0, 0, 0)) {
    return "http://" + dns.toString() + ":8000";
  }

  return String();
}

void initUserDatabase() {
  if (!LittleFS.exists("/users.json")) {
    File f = LittleFS.open("/users.json", FILE_WRITE);
    f.print("{\"admin\":{\"pw\":\"VestaAdmin\",\"role\":\"instructor\"},\"student1\":{\"pw\":\"hvac2026\",\"role\":\"student\"}}");
    f.close();
  }
}

void loadWiFiConfig() {
  if (LittleFS.exists("/wifi.json")) {
    File f = LittleFS.open("/wifi.json", FILE_READ);
    if (f) {
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, f);
      f.close();
      if (!error && !doc["ssid"].isNull()) {
        wifi_ssid = doc["ssid"].as<String>();
        wifi_pass = doc["pass"].as<String>();
      }
    }
  }
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
  if (fault_active[48]) return "Stuck Outdoor TXV";
  if (fault_active[42]) return "Clogged TXV";
  if (fault_active[43]) return "Clogged Piston";
  if (fault_active[49]) return "RV Bypassing";
  if (fault_active[44]) return "Compressor Internal Bypass";
  if (fault_active[45]) return "Inefficient Compressor";
  
  if (fault_active[24] || sim_active[1] || sim_active[2] || sim_active[6]) return "Failed Indoor Blower";
  if (fault_active[6] || sim_active[3] || sim_active[4]) return "Failed Condenser Fan";
  if (fault_active[12] || sim_active[5] || sim_active[13]) return "Stuck Reversing Valve";
  if (sim_active[15] || fault_active[31] || fault_active[4]) return "Failed Compressor / Overload";
  if (fault_active[15] || fault_active[16] || fault_active[17] || fault_active[18] || fault_active[19] || fault_active[20] || fault_active[21] || fault_active[22] || fault_active[23] || sim_active[12]) return "Failed Heat Strip Element";
  if (fault_active[32] || fault_active[33] || fault_active[34] || sim_active[8] || sim_active[9] || sim_active[10] || sim_active[11]) return "Defective Sequencer";
  if (sim_active[7]) return "Tripped Safety Limit";

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

  JsonDocument doc;
  
  doc["w_call"] = state_w ? true : false;
  doc["y_call"] = state_y ? true : false;
  doc["o_call"] = state_o ? true : false;
  doc["g_call"] = state_g ? true : false;
  
  doc["temp"] = 85.0;
  doc["hs1_t1"] = hs1_t1_on ? true : false;
  doc["hs1_t23"] = hs1_t23_on ? true : false;
  doc["hs2_t1"] = hs2_t1_on ? true : false;
  doc["hs2_t23"] = hs2_t23_on ? true : false;
  doc["hs3"] = hs3_on ? true : false;
  
  doc["wifi_rssi"] = is_ap_mode ? 0 : WiFi.RSSI();
  doc["ram"] = ESP.getFreeHeap();
  doc["uptime"] = uptime_str;
  doc["clients"] = ws.count();
  doc["diagnosis"] = latest_diagnosis;
  doc["student_score"] = student_score;
  doc["work_history"] = work_history_log;
  doc["refrigerant"] = current_refrigerant;
  doc["id_is_txv"] = id_is_txv ? 1 : 0; 
  doc["od_is_txv"] = od_is_txv ? 1 : 0;
  doc["is_b_type"] = is_b_type ? 1 : 0; 
  doc["indoor_metering"] = id_is_txv ? "TXV" : "Piston";
  doc["outdoor_metering"] = od_is_txv ? "TXV" : "Piston";
  
  doc["set_od"] = round(set_od_temp * 10.0) / 10.0;
  doc["set_id"] = round(set_id_temp * 10.0) / 10.0;
  doc["set_rh"] = round(set_rh * 10.0) / 10.0; 
  
  doc["is_defrosting"] = is_defrosting ? 1 : 0; 
  doc["force_defrost"] = force_defrost ? 1 : 0;
  
  doc["phys_lps"] = phys_lps_tripped ? 1 : 0;
  doc["phys_hps"] = phys_hps_tripped ? 1 : 0;
  doc["lps_open"] = (phys_lps_tripped || fault_active[7] || fault_active[26]) ? 1 : 0;
  doc["hps_open"] = (phys_hps_tripped || fault_active[8] || fault_active[27]) ? 1 : 0;
  doc["defrost_sensor"] = fault_active[9] ? 1 : 0; 
  
  doc["od_low_press"] = round((sim_od_low_press + low_noise) * 10.0) / 10.0;
  doc["od_high_press"] = round((sim_od_high_press + high_noise) * 10.0) / 10.0;
  doc["od_discharge_press"] = round((sim_od_high_press + high_noise) * 10.0) / 10.0;
  doc["od_liquid_press"] = round((sim_od_liquid_press + liquid_noise) * 10.0) / 10.0; // Fixed: Broadcast separate liquid pressure channel variable over WebSockets
  doc["od_suction_temp"] = round(sim_od_suction_temp * 10.0) / 10.0;
  doc["od_liquid_temp"] = round(sim_od_liquid_temp * 10.0) / 10.0;
  doc["od_ambient"] = round(sim_od_ambient * 10.0) / 10.0;
  doc["od_discharge_temp"] = round(sim_od_discharge * 10.0) / 10.0;
  doc["od_high_temp"] = round(sim_od_discharge * 10.0) / 10.0; // Alias for frontends expecting high_temp
  
  doc["id_return_temp"] = round(sim_id_return_temp * 10.0) / 10.0;
  doc["id_supply_temp"] = round(sim_id_supply_temp * 10.0) / 10.0;
  doc["id_ambient"] = round(sim_id_ambient * 10.0) / 10.0; 
  doc["id_rh"] = round(sim_id_rh * 10.0) / 10.0;
  doc["id_suction_temp"] = round((sim_od_suction_temp + 1.2) * 10.0) / 10.0; 
  doc["id_liquid_temp"] = round((sim_od_liquid_temp - 1.2) * 10.0) / 10.0;        
  
  doc["comp_amps"] = round(sim_comp_amps * 10.0) / 10.0;
  doc["od_fan_amps"] = round(sim_od_fan_amps * 10.0) / 10.0;
  doc["id_fan_amps"] = round(sim_id_fan_amps * 10.0) / 10.0;
  doc["hs_amps"] = round(sim_hs_amps * 10.0) / 10.0;

  doc["active_scenario"] = active_scenario;
  doc["student_score"] = student_score;
  doc["reset_counter"] = reset_counter;
  doc["ble_login_status"] = ble_login_status;
  doc["trainer_type"] = TRAINER_TYPE;
  bool identity_pin_live_is_low = (digitalRead(PIN_IDENTITY) == LOW);
  doc["identity_pin_gpio"] = PIN_IDENTITY;
  doc["identity_pin_boot_level"] = identity_pin_boot_is_low ? 0 : 1;
  doc["identity_pin_live_level"] = identity_pin_live_is_low ? 0 : 1;
  doc["identity_reboot_required"] = (identity_pin_live_is_low != identity_pin_boot_is_low) ? 1 : 0;
  doc["identity_latched_on_boot"] = true;
  if (active_trainer_type == STRAIGHT_AC_FURNACE) {
    doc["furnace_cfm"] = round(furnace_physics.getSimulatedCfm() * 10.0f) / 10.0f;
    doc["furnace_static_pressure"] = round(furnace_physics.getStaticPressure() * 100.0f) / 100.0f;
    doc["furnace_telemetry_state"] = furnace_physics.getTelemetryState();
    doc["furnace_flame_active"] = furnace_physics.isFlameActive() ? 1 : 0;
    doc["furnace_blower_running"] = furnace_physics.isBlowerRunning() ? 1 : 0;
    doc["furnace_high_limit"] = furnace_physics.isHighLimitTripped() ? 1 : 0;
    doc["gas_valve_monitor"] = readOptoActiveLow(PIN_GAS_VALVE_MONITOR) ? 1 : 0;
    doc["blower_monitor"] = readOptoActiveLow(PIN_BLOWER_MONITOR) ? 1 : 0;
    doc["relay_inducer"] = furnace_controller.isInducerOn() ? 1 : 0;
    doc["relay_igniter"] = furnace_controller.isIgniterOn() ? 1 : 0;
    doc["relay_gas_valve"] = furnace_controller.isGasValveOn() ? 1 : 0;
    doc["relay_heat_blower"] = furnace_controller.isHeatBlowerOn() ? 1 : 0;
    doc["furnace_state"] = furnace_controller.getFurnaceState();
  }
  if (ble_login_status == "success" && authToken.length() > 0) {
    doc["auth_token"] = authToken;
  }

  // Export full fault/simulation bitfields so external instructor UIs can mirror every toggle state.
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

  // Export relay/heat-strip states with IDs matching instructor controls.
  doc["hs1_t1"] = hs1_t1_on ? 1 : 0;
  doc["hs1_t2"] = hs1_t23_on ? 1 : 0;
  doc["hs1_t3"] = hs1_t23_on ? 1 : 0;
  doc["hs2_t1"] = hs2_t1_on ? 1 : 0;
  doc["hs2_t2"] = hs2_t23_on ? 1 : 0;
  doc["hs2_t3"] = hs2_t23_on ? 1 : 0;
  doc["hs3_t1"] = hs3_on ? 1 : 0;
  doc["hs3_t2"] = hs3_on ? 1 : 0;

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
  JsonDocument db;
  DeserializationError error = deserializeJson(db, f);
  f.close();
  if (!error && !db[user].isNull() && db[user]["pw"].as<String>().equalsIgnoreCase(pass)) {
    return true;
  }
  return false;
}

String getUserRole(String user) {
  user.toLowerCase();
  File f = LittleFS.open("/users.json", FILE_READ);
  if (!f) return "student";
  JsonDocument db;
  DeserializationError error = deserializeJson(db, f);
  f.close();
  if (!error && !db[user].isNull() && !db[user]["role"].isNull()) {
    return db[user]["role"].as<String>();
  }
  return "student";
}

String generateAuthToken() {
  uint32_t valueA = esp_random();
  uint32_t valueB = esp_random();
  char buf[24];
  snprintf(buf, sizeof(buf), "%08X%08X", valueA, valueB);
  return String(buf);
}

void forwardDiagnosisToEngine(const String& diagnosis) {
  if (engine_base_url.length() == 0) {
    Serial.println("Cannot forward diagnosis, engine URL unknown.");
    return;
  }

  HTTPClient http;
  // URL encode the diagnosis string to handle spaces.
  String diagnosis_encoded = diagnosis;
  diagnosis_encoded.replace(" ", "%20");

  String url = engine_base_url + "/api/submit?edge_id=" + EDGE_ID + "&diagnosis=" + diagnosis_encoded;
  http.begin(url);
  
  int httpCode = http.POST(""); // Send empty body as data is now in the query string.

  Serial.printf("Forwarded diagnosis to engine, status: %d\n", httpCode);
  http.end();
}

class MyCallbacks: public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo& connInfo) override {
    std::string rxValue = pCharacteristic->getValue();
    if (rxValue.length() > 0) {
      Serial.printf("BLE Received Payload: %s\n", rxValue.c_str());

      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, rxValue.c_str());

      if (!error) {
        if (!doc["force_defrost"].isNull()) {
          force_defrost = doc["force_defrost"].as<bool>();
          Serial.printf("Set force_defrost: %d\n", force_defrost);
        }
        if (!doc["reset_score"].isNull()) {
          student_score = 100;
          work_history_log = "";
          latest_diagnosis = "None";
          Serial.println("Reset Score Command Received over BLE");
        }
        if (!doc["diagnosis"].isNull()) {
          String submitted = doc["diagnosis"].as<String>();
          Serial.printf("Submitting Diagnosis over BLE: %s\n", submitted.c_str());
          forwardDiagnosisToEngine(submitted);
        }
        
        // --- NEW: BLE USER LOGIN CHECK ---
        if (!doc["user"].isNull() && !doc["pass"].isNull()) {
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
  // Use the global EDGE_LABEL, which is set correctly in applyTrainerIdentityMetadata.
  // This avoids using a local variable that goes out of scope, which was the cause of the bug.
  NimBLEDevice::init(EDGE_LABEL.c_str());
  NimBLEDevice::setMTU(512); // Request maximum MTU for faster/larger payload transfers
  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  NimBLEService *pService = pServer->createService(SERVICE_UUID);
  pTxCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID_TX, NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic *pRxCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID_RX, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  pRxCharacteristic->setCallbacks(new MyCallbacks());

  // Start the server
  pServer->start(); 

  // --- START SERVER ADVERTISING ---
  NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setName(EDGE_LABEL.c_str());
  pAdvertising->enableScanResponse(true); 
  pAdvertising->start();
}

void reset_all_faults_and_sims() {
  on_hs1_t1 = 0; off_hs1_t1 = 0; hs1_t1_on = false;
  on_hs1_t23 = 0; off_hs1_t23 = 0; hs1_t23_on = false;
  on_hs2_t1 = 0; off_hs2_t1 = 0; hs2_t1_on = false;
  on_hs2_t23 = 0; off_hs2_t23 = 0; hs2_t23_on = false;
  on_hs3 = 0; off_hs3 = 0; hs3_on = false;
  seq3_timer_started = false;
  
  force_defrost = false; 
  override_defrost_sensor = -1; 
  heat_runtime_start = 0;       

  active_scenario = 0;
  scenario_start_time = 0;

  phys_lps_tripped = false;
  phys_hps_tripped = false;
  
  comp_start_time = 0;
  last_comp_state = false;

  for(int i = 0; i < 16; i++) { sim_active[i] = false; sim_timer[i] = 0; sim_step[i] = 0; }
  for(int i = 0; i < 55; i++) { fault_active[i] = false; } 
  
  limit_trip_count = 0;

  if (i2c_boards_present) {
    for (int i = 0; i < 16; i++) { board_1.digitalWrite(i, HIGH); board_2.digitalWrite(i, HIGH); board_3.digitalWrite(i, HIGH); }
    board_1.digitalWrite(0, LOW); board_1.digitalWrite(4, LOW); board_1.digitalWrite(8, LOW);
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

void onMqttConnect(bool sessionPresent) {
  Serial.println("Connected to MQTT.");
  String topic = "trainer/" + EDGE_ID + "/command";
  mqttClient.subscribe(topic.c_str(), 1);
  Serial.printf("Subscribed to MQTT topic: %s\n", topic.c_str());
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  Serial.println("Disconnected from MQTT.");
  if (WiFi.isConnected()) {
    xTimerStart(mqttReconnectTimer, 0);
  }
}

void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
  payload[len] = '\0';
  Serial.printf("MQTT Message Received: [%s] %s\n", topic, payload);

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    Serial.printf("MQTT JSON deserialize error: %s\n", error.c_str());
    return;
  }

  const char* action = doc["action"];
  if (action && strcmp(action, "reboot") == 0) {
    Serial.println("Reboot command received via MQTT. Rebooting in 150ms.");
    pending_reboot = true;
    reboot_timer = millis() + 150;
  } else if (action && strcmp(action, "reset") == 0) {
    Serial.println("Reset command received via MQTT.");
    reset_all_faults_and_sims();
    force_telemetry_update = true;
  }
}

void setup() {
  Serial.begin(115200);
  // --- FIX: Determine identity FIRST, so all subsequent services get the right name ---
  detectTrainerTypeAtBoot();
  applyTrainerIdentityMetadata();

  status_led.begin(); status_led.setBrightness(100);
  setStatusLeds(getModeConnectedLedColor());

  setupBLE(); // Initialize BLE early to secure memory and RF coexistence before WiFi

  if(!LittleFS.begin(true)) { Serial.println("LittleFS Mount Failed"); return; }
  initUserDatabase();

  loadWiFiConfig();
  if (wifi_ssid.length() == 0) {
    wifi_ssid = "ComfortSC";
    wifi_pass = "8037945526";
  }

  Serial.printf("Trainer identity pin GPIO%d => mode: %s\n", PIN_IDENTITY, TRAINER_TYPE);

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);

  Serial.printf("WiFi attempting SSID: '%s'\n", wifi_ssid.c_str());
  setStatusLeds(status_led.Color(255, 180, 0)); // Yellow while connecting to Wi-Fi
  wifi_disconnected_since = millis();
  WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) { delay(500); attempts++; }

  if (WiFi.status() != WL_CONNECTED && wifi_ssid != "ComfortSC") {
    Serial.println("Primary WiFi failed. Falling back to default SSID ComfortSC.");
    wifi_ssid = "ComfortSC";
    wifi_pass = "8037945526";
    WiFi.disconnect(false, false);
    delay(150);
    WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
    attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) { delay(500); attempts++; }
  }

  if (WiFi.status() == WL_CONNECTED) {
    is_ap_mode = false;
    wifi_was_connected = true;
    wifi_disconnected_since = 0;
    wifi_connected_flash_until = millis() + WIFI_CONNECTED_FLASH_MS;
    Serial.println("\nWiFi connected.");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    // --- FIX: Discover engine URL once at startup and use it for both HTTP and MQTT ---
    engine_base_url = discoverEngineBaseUrl(true); // Allow stored value first
    if (engine_base_url.length() == 0) {
      engine_base_url = discoverEngineBaseUrl(false); // Force discovery if not stored
    }
    if (engine_base_url.length() > 0) {
      saveEngineBaseUrl(engine_base_url);
    }

    Serial.printf("Engine endpoint: %s\n", engine_base_url.c_str());
    if (MDNS.begin(OTA_HOSTNAME.c_str())) {
      Serial.printf("MDNS responder started! Domain: %s.local\n", OTA_HOSTNAME.c_str());
      MDNS.addService("http", "tcp", 80); 
    }
    configTzTime("EST5EDT,M3.2.0,M11.1.0", "pool.ntp.org", "time.nist.gov");

    // --- MQTT SETUP ---
    mqttReconnectTimer = xTimerCreate("mqttTimer", pdMS_TO_TICKS(2000), pdFALSE, (void*)0, reinterpret_cast<TimerCallbackFunction_t>(connectToMqtt));
    IPAddress mqtt_host;
    String mqtt_host_str = engine_base_url;
    mqtt_host_str.replace("http://", "");
    int port_index = mqtt_host_str.indexOf(':');
    if (port_index != -1) { mqtt_host_str = mqtt_host_str.substring(0, port_index); }
    if (mqtt_host_str.length() > 0 && mqtt_host.fromString(mqtt_host_str)) {
        mqttClient.setServer(mqtt_host, 1883);
        mqttClient.onConnect(onMqttConnect);
        mqttClient.onDisconnect(onMqttDisconnect);
        mqttClient.onMessage(onMqttMessage);
        connectToMqtt();
    } else {
      Serial.printf("Failed to configure MQTT host from engine URL: %s\n", engine_base_url.c_str());
    }

  } else {
    is_ap_mode = true;
    wifi_was_connected = false;
    WiFi.mode(WIFI_AP);
    setup_ap_active = WiFi.softAP("Vesta Core Trainer", "8037945526"); 
    dnsServer.start(53, "*", WiFi.softAPIP()); 
  }
  
  ArduinoOTA.setHostname(OTA_HOSTNAME.c_str());
  ArduinoOTA.setPort(3232);
  ArduinoOTA.setPassword("Mitchell2019!");
  Serial.printf("Arduino OTA hostname: %s.local\n", OTA_HOSTNAME.c_str());
  ArduinoOTA.onStart([]() {
    ota_in_progress = true;
    setStatusLeds(status_led.Color(0, 0, 255), true, 255); // Main + OTA marker blue
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    ota_in_progress = false;
    setStatusLeds(status_led.Color(0, 255, 0)); // Green on success
    Serial.println("\nEnd");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    ota_in_progress = false;
    setStatusLeds(status_led.Color(255, 0, 0)); // Red on error
    Serial.printf("OTA Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    uint8_t blue_val = 50 + ((progress * 205) / total); // Ramps brightness dynamically from dim to max
    setStatusLeds(status_led.Color(0, 0, blue_val), true, blue_val);
  });
  ArduinoOTA.begin();

  pinMode(PIN_W, INPUT_PULLUP); pinMode(PIN_Y, INPUT_PULLUP); pinMode(PIN_O, INPUT_PULLUP); pinMode(PIN_G, INPUT_PULLUP);
  pinMode(PIN_BLOWER_MONITOR, INPUT_PULLUP);
  pinMode(PIN_GAS_VALVE_MONITOR, INPUT_PULLUP);

  bool wire_ok = Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  if (!wire_ok) {
    i2c_boards_present = false;
    Serial.printf("I2C init failed on SDA=%d/SCL=%d. Running in no-relay mode.\n", I2C_SDA_PIN, I2C_SCL_PIN);
  } else {
    detectI2CBoards();
  }
  if (i2c_boards_present) {
    board_1.begin();
    board_2.begin();
    board_3.begin();
  }
  reset_all_faults_and_sims();
  if (active_trainer_type == STRAIGHT_AC_FURNACE) {
    furnace_controller.begin();
    furnace_physics.begin();
    furnace_physics.setAmbient(set_od_temp, set_id_temp, set_rh);
    furnace_physics.setRefrigerant(current_refrigerant, id_is_txv);
    syncFurnaceTelemetryToUnifiedState();
  }

  ws.onEvent([](AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) { client->text(getStatusJSON()); }
  });
  server.addHandler(&ws);

  // --- OTA Update via Web Browser ---
  server.on("/api/identity/set", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("edge_id") && request->hasParam("label")) {
      EDGE_ID = request->getParam("edge_id")->value();
      EDGE_LABEL = request->getParam("label")->value();
      is_identity_resolved = true;
      // Persist this identity for next boot if needed in future
      Serial.printf("Identity resolved from backend: ID=%s, Label=%s\n", EDGE_ID.c_str(), EDGE_LABEL.c_str());
      setupBLE(); // Re-initialize BLE to broadcast the new name
      force_telemetry_update = true;
      request->send(200, "text/plain", "OK");
      return;
    }
    request->send(400, "text/plain", "Bad Request");
  });

  server.on("/api/command", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("action")) {
      String action = request->getParam("action")->value();
      if (action == "reset") {
        reset_all_faults_and_sims();
        force_telemetry_update = true;
        request->send(200, "text/plain", "OK");
        return;
      }
    }
    request->send(400, "text/plain", "Bad Request");
  });



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
      setStatusLeds(status_led.Color(0, 0, 255), true, 255);
      Serial.printf("Update Start: %s\n", filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { Update.printError(Serial); }
    }
    if (len) { if (Update.write(data, len) != len) { Update.printError(Serial); } }
    if (final) {
      ota_in_progress = false;
      // Accept exact uploaded image length; do not require filling the entire OTA slot.
      if (Update.end(true)) {
        setStatusLeds(status_led.Color(0, 255, 0));
        Serial.printf("Update Success: %uB\n", index + len);
      } else {
        setStatusLeds(status_led.Color(255, 0, 0));
        Update.printError(Serial);
      }
    }
  });

  server.on("/api/metering", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("id_txv")) { id_is_txv = (request->getParam("id_txv")->value() == "1"); }
    if (request->hasParam("od_txv")) { od_is_txv = (request->getParam("od_txv")->value() == "1"); }
    force_telemetry_update = true; 
    request->send(200, "text/plain", "OK");
  });

  server.on("/api/refrigerant", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("type")) {
      current_refrigerant = request->getParam("type")->value();
      force_pressure_snap = true; 
      force_telemetry_update = true; 
      request->send(200, "text/plain", "OK");
    } else { request->send(400, "text/plain", "Bad Request"); }
  });

  server.on("/api/ob_preference", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("type")) {
      String type = request->getParam("type")->value();
      is_b_type = (type == "B");
      force_telemetry_update = true; 
      request->send(200, "text/plain", "OK");
    } else { request->send(400, "text/plain", "Bad Request"); }
  });

  server.on("/api/users/add", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
  [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, data, len);
    if (!error && !doc["user"].isNull() && !doc["pass"].isNull()) {
      File f = LittleFS.open("/users.json", FILE_READ);
      JsonDocument db; deserializeJson(db, f); f.close();
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
      JsonDocument db; DeserializationError error = deserializeJson(db, f); f.close();
      if (!error && !db[user].isNull() && db[user]["pw"].as<String>().equalsIgnoreCase(pass)) {
        String role = db[user]["role"].as<String>();
        logLogin(user, role); 
        request->send(200, "text/plain", role);
      } else { request->send(401, "text/plain", "DENIED"); }
    } else { request->send(400, "text/plain", "Bad Request"); }
  });

  server.on("/wifi-setup", HTTP_GET, [](AsyncWebServerRequest *request){
    String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'></head>"
                  "<body style='font-family: sans-serif; padding: 20px;'>"
                  "<h2>Wi-Fi Setup</h2>"
                  "<form action='/wifi-save' method='POST'>"
                  "<label>SSID:</label><br><input type='text' name='ssid' value='" + wifi_ssid + "' style='width:100%; max-width:300px;'><br><br>"
                  "<label>Password:</label><br><input type='password' name='pass' style='width:100%; max-width:300px;'><br><br>"
                  "<input type='submit' value='Save & Reconnect' style='padding: 10px 20px;'></form>"
                  "</body></html>";
    request->send(200, "text/html", html);
  });

  server.on("/wifi-save", HTTP_POST, [](AsyncWebServerRequest *request){
    if(request->hasParam("ssid", true) && request->hasParam("pass", true)) {
      JsonDocument doc;
      doc["ssid"] = request->getParam("ssid", true)->value();
      doc["pass"] = request->getParam("pass", true)->value();
      File f = LittleFS.open("/wifi.json", FILE_WRITE); serializeJson(doc, f); f.close();
      wifi_ssid = doc["ssid"].as<String>();
      wifi_pass = doc["pass"].as<String>();
      is_ap_mode = false;
      WiFi.mode(WIFI_STA);
      WiFi.disconnect(false, false);
      delay(150);
      WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
      request->send(200, "text/html", "<html><body style='font-family:sans-serif;'><h2>Saved! Reconnecting to Wi-Fi now (no reboot required).</h2></body></html>");
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

  server.on("/api/reset", HTTP_POST, [](AsyncWebServerRequest *request){ reset_all_faults_and_sims(); force_telemetry_update = true; request->send(200, "text/plain", "OK"); });

  server.on("/api/submit", HTTP_POST, [](AsyncWebServerRequest *request){
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
    if (request->hasParam("od") && request->hasParam("id")) {
      set_od_temp = request->getParam("od")->value().toFloat();
      set_id_temp = request->getParam("id")->value().toFloat();
      if(request->hasParam("rh")) set_rh = request->getParam("rh")->value().toFloat(); 
      force_telemetry_update = true; 
      request->send(200, "text/plain", "OK");
    } else { request->send(400, "text/plain", "Bad Request"); }
  });

  server.on("/api/engine", HTTP_GET, [](AsyncWebServerRequest *request){
    String payload = "{";
    payload += "\"engineBaseUrl\":\"" + engine_base_url + "\"";
    payload += "}";
    request->send(200, "application/json", payload);
  });

  server.on("/api/engine", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!request->hasParam("url")) {
      request->send(400, "text/plain", "Missing url");
      return;
    }

    String submitted = request->getParam("url")->value();
    submitted.trim();
    if (!submitted.startsWith("http://") && !submitted.startsWith("https://")) {
      request->send(400, "text/plain", "URL must start with http:// or https://");
      return;
    }

    engine_base_url = submitted;
    saveEngineBaseUrl(engine_base_url);
    next_engine_discovery_ms = 0;
    engine_heartbeat_timer = 0;
    request->send(200, "text/plain", "OK");
  });

  server.on("/api/identity", HTTP_GET, [](AsyncWebServerRequest *request){
    bool identity_pin_live_is_low = (digitalRead(PIN_IDENTITY) == LOW);
    bool reboot_required = (identity_pin_live_is_low != identity_pin_boot_is_low);

    JsonDocument doc;
    doc["trainer_type"] = TRAINER_TYPE;
    doc["identity_pin_gpio"] = PIN_IDENTITY;
    doc["identity_pin_boot_level"] = identity_pin_boot_is_low ? 0 : 1;
    doc["identity_pin_live_level"] = identity_pin_live_is_low ? 0 : 1;
    doc["identity_reboot_required"] = reboot_required;
    doc["note"] = "GPIO14 identity is sampled once at boot. Reboot required after strap change.";

    String payload;
    serializeJson(doc, payload);
    request->send(200, "application/json", payload);
  });

  server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest *request){
    pending_reboot = true;
    reboot_timer = millis() + 150;
    request->send(200, "text/plain", "Reboot scheduled");
  });

  server.on("/api/led/mode", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!request->hasParam("mode")) {
      request->send(400, "text/plain", "Missing mode");
      return;
    }

    String mode = request->getParam("mode")->value();
    mode.toLowerCase();

    if (mode == "status") {
      led_control_mode = LED_MODE_STATUS;
      setStatusLeds(getModeConnectedLedColor());
      request->send(200, "text/plain", "OK");
      return;
    }

    if (mode == "test") {
      if (request->hasParam("interval")) {
        int interval = request->getParam("interval")->value().toInt();
        if (interval >= 100 && interval <= 5000) {
          led_test_interval_ms = static_cast<uint32_t>(interval);
        }
      }
      led_control_mode = LED_MODE_TEST;
      led_test_index = 0;
      led_test_color_step = 0;
      led_timer = 0;
      request->send(200, "text/plain", "OK");
      return;
    }

    if (mode == "manual") {
      led_control_mode = LED_MODE_MANUAL;
      applyManualLeds();
      request->send(200, "text/plain", "OK");
      return;
    }

    request->send(400, "text/plain", "Invalid mode. Use status|test|manual");
  });

  server.on("/api/led/set", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!request->hasParam("r") || !request->hasParam("g") || !request->hasParam("b")) {
      request->send(400, "text/plain", "Missing r,g,b");
      return;
    }

    uint8_t r = clampColorByte(request->getParam("r")->value().toInt());
    uint8_t g = clampColorByte(request->getParam("g")->value().toInt());
    uint8_t b = clampColorByte(request->getParam("b")->value().toInt());
    uint32_t color = status_led.Color(r, g, b);

    if (request->hasParam("index")) {
      int index = request->getParam("index")->value().toInt();
      if (index < 0 || index >= status_led.numPixels()) {
        request->send(400, "text/plain", "Index out of range");
        return;
      }
      led_manual_colors[index] = color;
    } else {
      for (uint16_t i = 0; i < status_led.numPixels(); i++) {
        led_manual_colors[i] = color;
      }
    }

    led_control_mode = LED_MODE_MANUAL;
    applyManualLeds();
    request->send(200, "text/plain", "OK");
  });

  server.on("/api/led/status", HTTP_GET, [](AsyncWebServerRequest *request){
    String payload = "{";
    payload += "\"mode\":\"" + String(getLedModeName()) + "\"";
    payload += ",\"pixels\":" + String(status_led.numPixels());
    payload += ",\"testIntervalMs\":" + String(led_test_interval_ms);
    payload += "}";
    request->send(200, "application/json", payload);
  });

  server.onNotFound([](AsyncWebServerRequest *request){
    if (request->method() == HTTP_OPTIONS) {
      request->send(200);
    } else if (is_ap_mode || setup_ap_active) { 
      request->redirect("http://" + WiFi.softAPIP().toString() + "/"); 
    } else { 
      request->send(404, "text/plain", "Not Found"); 
    }
  });

  server.on("/api/toggle", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("id") && request->hasParam("state")) {
      String id = request->getParam("id")->value();
      bool state = request->getParam("state")->value().toInt() == 1;
      int val_inv = state ? LOW : HIGH; int val_nor = state ? HIGH : LOW; 
      
      if (id == "force_defrost") { force_defrost = state; } else if (id == "reset_score") {
          student_score = 100;
          work_history_log = "";
          latest_diagnosis = "None";
      } else if (id.startsWith("sim_")) {
        int simNum = id.substring(4).toInt();
        if (simNum >= 1 && simNum <= 15) { sim_active[simNum] = state; sim_timer[simNum] = 0; sim_step[simNum] = 0; 
          if (!state) { 
            if (simNum == 1) { board_1.digitalWrite(12, HIGH); board_3.digitalWrite(6, HIGH); } 
            if (simNum == 2) { board_1.digitalWrite(12, HIGH); board_1.digitalWrite(1, HIGH); board_1.digitalWrite(5, HIGH); board_1.digitalWrite(9, HIGH); } 
            if (simNum == 3) { board_3.digitalWrite(5, HIGH); board_3.digitalWrite(7, HIGH); } 
            if (simNum == 4) { board_3.digitalWrite(5, HIGH); board_3.digitalWrite(6, HIGH); } 
            if (simNum == 5) { board_3.digitalWrite(11, HIGH); } 
            if (simNum == 6) { board_1.digitalWrite(12, HIGH); } 
            if (simNum == 7) { board_1.digitalWrite(2, HIGH); board_1.digitalWrite(6, HIGH); board_1.digitalWrite(10, HIGH); board_3.digitalWrite(7, HIGH); } 
            if (simNum == 8) { board_1.digitalWrite(3, HIGH); } 
            if (simNum == 9) { board_1.digitalWrite(7, HIGH); }  
            if (simNum == 10){ board_1.digitalWrite(11, HIGH); }  
            if (simNum == 13){ board_3.digitalWrite(11, HIGH); } 
            if (simNum == 14){ board_2.digitalWrite(12, HIGH); } 
            if (simNum == 15){ board_2.digitalWrite(14, HIGH); } 
          }
        }
      } else if (id.startsWith("f")) {
        int f = id.substring(1).toInt();
        if (f == 9) {
            override_defrost_sensor = state ? 1 : 0;
            fault_active[9] = state;
            board_3.digitalWrite(8, state ? LOW : HIGH);
        } else {
            fault_active[f] = state; 
            switch(f) {
              case 15: board_1.digitalWrite(0, val_nor); break;
              case 18: board_1.digitalWrite(1, val_inv); break;
              case 21: board_1.digitalWrite(2, val_inv); break;
              case 32: board_1.digitalWrite(3, val_inv); break;
              case 16: board_1.digitalWrite(4, val_nor); break;
              case 19: board_1.digitalWrite(5, val_inv); break;
              case 22: board_1.digitalWrite(6, val_inv); break;
              case 33: board_1.digitalWrite(7, val_inv); break;
              case 17: board_1.digitalWrite(8, val_nor); break;
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
        }
      } else {
        if (id == "hs1_t1") board_2.digitalWrite(0, val_inv); 
        else if (id == "hs1_t2") board_2.digitalWrite(1, val_inv); 
        else if (id == "hs1_t3") board_2.digitalWrite(2, val_inv); 
        else if (id == "hs2_t1") board_2.digitalWrite(3, val_inv); 
        else if (id == "hs2_t2") board_2.digitalWrite(4, val_inv); 
        else if (id == "hs2_t3") board_2.digitalWrite(5, val_inv); 
        else if (id == "hs3_t1") board_2.digitalWrite(6, val_inv); 
        else if (id == "hs3_t2") board_2.digitalWrite(7, val_inv); 
        else if (id == "relay_compressor_motor") board_2.digitalWrite(8, val_inv); 
        else if (id == "relay_compressor_contactor") board_2.digitalWrite(9, val_inv); 
        else if (id == "relay_reversing_valve") board_2.digitalWrite(10, val_inv); 
        else if (id == "relay_condenser_y") board_2.digitalWrite(11, val_inv); 
        else if (id == "relay_y_wire_interlock") board_2.digitalWrite(12, val_inv);
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

  BaseType_t comm_created = xTaskCreatePinnedToCore(
    commTask,
    "comm_task",
    8192,
    nullptr,
    1,
    &comm_task_handle,
    0
  );
  BaseType_t control_created = xTaskCreatePinnedToCore(
    controlTask,
    "control_task",
    8192,
    nullptr,
    1,
    &control_task_handle,
    1
  );

  dual_core_runtime_enabled = (comm_created == pdPASS && control_created == pdPASS);
  if (dual_core_runtime_enabled) {
    Serial.println("Dual-core runtime enabled: comm=core0, control=core1");
  } else {
    Serial.println("Dual-core task creation failed, using single-core loop fallback.");
  }
}

void sendEngineHeartbeat() {
  if (!engine_link_enabled || WiFi.status() != WL_CONNECTED) return;
  uint32_t now = millis();

  bool changed = (state_w != hb_last_w) ||
                 (state_y != hb_last_y) ||
                 (state_o != hb_last_o) ||
                 (state_g != hb_last_g) ||
                 (phys_lps_tripped != hb_last_lps) ||
                 (phys_hps_tripped != hb_last_hps);

  if (!changed && now < engine_heartbeat_timer) return;
  engine_heartbeat_timer = now + ENGINE_HEARTBEAT_INTERVAL_MS;

  if (engine_base_url.length() == 0) {
    if (now < next_engine_discovery_ms) return;
    engine_base_url = discoverEngineBaseUrl(false);
    next_engine_discovery_ms = now + ENGINE_DISCOVERY_RETRY_MS;
    if (engine_base_url.length() == 0) return;
  }

  auto postHeartbeat = [](const String& baseUrl, const String& payload) -> bool {
    if (baseUrl.length() == 0) return false;
    HTTPClient http;
    String url = baseUrl + "/api/edge/heartbeat";
    http.setTimeout(1200);
    if (!http.begin(url)) return false;
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(payload);
    http.end();
    return code >= 200 && code < 300;
  };

  uint32_t uptime_sec = millis() / 1000;
  char uptime_str[15];
  snprintf(uptime_str, sizeof(uptime_str), "%02d:%02d:%02d", (int)(uptime_sec / 3600), (int)((uptime_sec % 3600) / 60), (int)(uptime_sec % 60));

  JsonDocument doc;
  doc["edge_id"] = EDGE_ID;
  doc["device_name"] = EDGE_LABEL;
  doc["w"] = state_w;
  doc["y"] = state_y;
  doc["o"] = state_o;
  doc["g"] = state_g;
  doc["phys_lps"] = phys_lps_tripped;
  doc["phys_hps"] = phys_hps_tripped;
  doc["trainer_type"] = TRAINER_TYPE;
  doc["wifi_rssi"] = WiFi.RSSI();
  doc["ram"] = ESP.getFreeHeap();
  doc["uptime"] = uptime_str;
  doc["temp"] = 0.0;

  // Add live identity pin status to notify UI if a reboot is needed after strap change.
  bool identity_pin_live_is_low = (digitalRead(PIN_IDENTITY) == LOW);
  doc["identity_reboot_required"] = (identity_pin_live_is_low != identity_pin_boot_is_low);

  String payload;
  serializeJson(doc, payload);

  if (postHeartbeat(engine_base_url, payload)) {
    saveEngineBaseUrl(engine_base_url);
    hb_last_w = state_w;
    hb_last_y = state_y;
    hb_last_o = state_o;
    hb_last_g = state_g;
    hb_last_lps = phys_lps_tripped;
    hb_last_hps = phys_hps_tripped;
    return;
  }

  String gatewayBase = "http://" + WiFi.gatewayIP().toString() + ":8000";
  if (gatewayBase != engine_base_url && postHeartbeat(gatewayBase, payload)) {
    engine_base_url = gatewayBase;
    saveEngineBaseUrl(engine_base_url);
    hb_last_w = state_w;
    hb_last_y = state_y;
    hb_last_o = state_o;
    hb_last_g = state_g;
    hb_last_lps = phys_lps_tripped;
    hb_last_hps = phys_hps_tripped;
    Serial.printf("Engine heartbeat switched to gateway endpoint: %s\n", engine_base_url.c_str());
    return;
  }

  if (now >= next_engine_discovery_ms) {
    String discoveredBase = discoverEngineBaseUrl(false);
    next_engine_discovery_ms = now + ENGINE_DISCOVERY_RETRY_MS;
    if (discoveredBase.length() > 0 && discoveredBase != engine_base_url && postHeartbeat(discoveredBase, payload)) {
      engine_base_url = discoveredBase;
      saveEngineBaseUrl(engine_base_url);
      hb_last_w = state_w;
      hb_last_y = state_y;
      hb_last_o = state_o;
      hb_last_g = state_g;
      hb_last_lps = phys_lps_tripped;
      hb_last_hps = phys_hps_tripped;
      Serial.printf("Engine heartbeat switched to discovered endpoint: %s\n", engine_base_url.c_str());
      return;
    }
  }

  Serial.printf("Engine heartbeat failed at %s\n", engine_base_url.c_str());
}

uint32_t heartbeat_timer = 0;
void handle_system_health() {
  uint32_t now = millis();
  if (now >= led_timer) {
    if (led_control_mode == LED_MODE_TEST) {
      led_timer = now + led_test_interval_ms;
      stepLedTestPattern();
    } else if (led_control_mode == LED_MODE_MANUAL) {
      led_timer = now + 1000;
      applyManualLeds();
    } else {
      if (ota_in_progress) {
        // Blue flash during OTA operations.
        bool on_phase = ((now / 400) % 2) == 0;
        led_timer = now + 200;
        setStatusLeds(on_phase ? status_led.Color(0, 0, 255) : 0, true, on_phase ? 255 : 0);
        if (now >= heartbeat_timer) { heartbeat_timer = now + 500; notifyClients(); }
        return;
      }

      uint32_t mainColor;
      led_timer = now + 500;
      if (is_ap_mode) {
        mainColor = status_led.Color(170, 0, 170); // Purple while AP mode is active
      } else if (WiFi.status() == WL_CONNECTED) {
        if (now < wifi_connected_flash_until) {
          bool on_phase = ((now / 1000) % 2) == 0; // Slow green flash on successful connect
          mainColor = on_phase ? status_led.Color(0, 255, 0) : 0;
        } else {
          // Normal connected mode: solid green
          mainColor = getModeConnectedLedColor();
        }
      } else {
        if (wifi_disconnected_since == 0 || (now - wifi_disconnected_since) < WIFI_CONNECTING_GRACE_MS) {
          mainColor = status_led.Color(255, 180, 0); // Yellow while attempting to connect/reconnect
        } else {
          mainColor = status_led.Color(255, 0, 0); // Red on persistent connection issue
        }
      }
      setStatusLeds(mainColor);
    }
  }
  if (now >= heartbeat_timer) { heartbeat_timer = now + 500; notifyClients(); } 
}

float add_noise(float base, float variance) {
  float r = (esp_random() % 1000) / 1000.0; 
  return base + ((r * (variance * 2.0)) - variance);
}

// ==========================================
// ðŸš€ DYNAMIC PHYSICS & FAULT ENGINE
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
  is_defrosting = force_defrost || (state_y && state_o && state_w && set_od_temp <= 65.0);
  
  bool y_broken = fault_active[2] || sim_active[14];
  bool y_circuit_intact = (state_y && !lps_open && !hps_open && !y_broken);
  bool contactor_pulled = y_circuit_intact || fault_active[30]; 
  bool is_compressor = contactor_pulled && !fault_active[31] && !sim_active[15]; 

  bool o_broken = fault_active[1];
  bool rv_fail = fault_active[12] || sim_active[5] || sim_active[13];
  bool actual_o = state_o && !o_broken;
  
  bool phys_cooling = false;
  bool phys_heating = false;
  
  if (is_b_type) {
      phys_heating = (is_compressor && actual_o && !rv_fail);
      phys_cooling = (is_compressor && (!actual_o || rv_fail));
  } else {
      phys_cooling = (is_compressor && actual_o && !rv_fail);
      phys_heating = (is_compressor && (!actual_o || rv_fail));
  }

  if (phys_heating && set_od_temp < 50.0) {
      if (heat_runtime_start == 0) heat_runtime_start = now;
      bool auto_defrost = (now - heat_runtime_start >= 180000); 
      if (override_defrost_sensor == -1) fault_active[9] = auto_defrost;
  } else { heat_runtime_start = 0; if (override_defrost_sensor == -1) fault_active[9] = false; }

  if (override_defrost_sensor == 1) fault_active[9] = true;
  else if (override_defrost_sensor == 0) fault_active[9] = false;
  if (i2c_boards_present) {
    board_3.digitalWrite(8, fault_active[9] ? LOW : HIGH);
  }

  if (is_defrosting && is_compressor) { phys_cooling = true; phys_heating = false; }

  bool id_fan_fail = fault_active[24] || sim_active[1] || sim_active[2] || sim_active[6];
  bool od_fan_fail = fault_active[6] || sim_active[3] || sim_active[4] || (is_defrosting && !fault_active[10]); 

  // --- FAULT MAPPING REDECLARATIONS ---
  bool fault_non_condensables = fault_active[40];
  bool fault_stuck_id_txv     = fault_active[41];
  bool fault_clogged_txv      = fault_active[42];
  bool fault_clogged_piston   = fault_active[43];
  bool fault_comp_bypass      = fault_active[44];
  bool fault_inefficient_comp = fault_active[45];
  bool fault_low_id_cfm       = fault_active[46];
  bool fault_high_id_cfm      = fault_active[47];
  bool fault_stuck_od_txv     = fault_active[48]; 
  bool fault_rv_bypass        = fault_active[49]; 

  float heat_boost = 0.0;
  bool hs1_broken = fault_active[15] || fault_active[18] || fault_active[21] || fault_active[32] || sim_active[8] || sim_active[12];
  bool hs2_broken = fault_active[16] || fault_active[19] || fault_active[22] || fault_active[33] || sim_active[9];
  bool hs3_broken = fault_active[17] || fault_active[20] || fault_active[23] || fault_active[34] || sim_active[10];

  if ((hs1_t23_on || sim_active[11]) && !hs1_broken) heat_boost += 13.0; 
  if (hs2_t23_on && !hs2_broken) heat_boost += 13.0; 
  if (hs3_on && !hs3_broken)     heat_boost += 13.0; 

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
    float bleed_rate = (id_is_txv && od_is_txv) ? 0.005f : 0.025f; 
    
    sim_od_high_press += (target_eq_press - sim_od_high_press) * bleed_rate;
    sim_od_low_press += (target_eq_press - sim_od_low_press) * bleed_rate;
    
    sim_od_suction_temp += (set_od_temp - sim_od_suction_temp) * 0.05f;
    sim_od_liquid_temp += (set_od_temp - sim_od_liquid_temp) * 0.05f;
    sim_od_discharge += (set_od_temp - sim_od_discharge) * 0.05f;
    target_supply = set_id_temp + heat_boost; 
    
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
    if (fault_rv_bypass) { target_low += 50.0f * ref_mult; target_high -= 80.0f * ref_mult; target_sh += 15.0f; }
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
    target_supply = (set_id_temp - 20.0f) + heat_boost + latent_heat_penalty; 

    if (fault_comp_bypass) sim_od_suction_temp += 35.0f; 
    if (fault_rv_bypass) sim_od_suction_temp += 45.0f; 
    if (fault_non_condensables) sim_od_liquid_temp -= 12.0f; 
    if (fault_clogged_txv || fault_clogged_piston) sim_od_liquid_temp -= 15.0f; 

    if (od_fan_fail) sim_od_discharge = add_noise(220.0f, 5.0f);
    if (id_fan_fail) sim_od_suction_temp = add_noise(25.0f, 0.5f); 

  } else if (phys_heating) {
    target_low = ((set_od_temp * 1.8f) + 25.0f) * ref_mult;   
    float base_head = (set_id_temp * 3.0f) + (set_od_temp * 1.5f) + 50.0f;
    float extreme_ambient_penalty = 0.0f;
    if (set_od_temp > 65.0f) { float excess = set_od_temp - 65.0f; extreme_ambient_penalty = (excess * excess * 1.2f); }
    target_high = (base_head + extreme_ambient_penalty) * ref_mult;  
    target_sh = od_is_txv ? 10.0f : 15.0f;

    line_friction_delta = id_fan_fail ? 7.0f : 24.0f; 

    if (fault_non_condensables) { target_high += 130.0f * ref_mult; target_low += 5.0f * ref_mult; line_friction_delta += 12.0f; }
    if (fault_stuck_od_txv) { target_low -= 35.0f * ref_mult; target_high -= 15.0f * ref_mult; target_sh = 35.0f; }
    if (fault_clogged_txv || fault_clogged_piston) { target_low -= 35.0f * ref_mult; target_high -= 15.0f * ref_mult; target_sh = 35.0f; }
    if (fault_comp_bypass) { target_low += 40.0f * ref_mult; target_high -= 75.0f * ref_mult; }
    if (fault_inefficient_comp) { target_low += 25.0f * ref_mult; target_high -= 45.0f * ref_mult; }
    if (fault_rv_bypass) { target_low += 50.0f * ref_mult; target_high -= 80.0f * ref_mult; target_sh += 15.0f; }
    if (fault_low_id_cfm) { target_high += 55.0f * ref_mult; target_supply += 18.0f; line_friction_delta += 10.0f; } 
    if (fault_high_id_cfm) { target_high -= 25.0f * ref_mult; target_supply -= 9.0f; line_friction_delta -= 6.0f; }

    float low_abs = sim_od_low_press + 14.7f;
    if (low_abs < 1.0f) low_abs = 1.0f;
    float comp_ratio = (sim_od_high_press + 14.7f) / low_abs;
    float vol_eff_penalty = (comp_ratio - 2.5f) * 4.0f; 
    if (vol_eff_penalty < 0.0f) vol_eff_penalty = 0.0f;

    target_low += vol_eff_penalty * ref_mult; 
    
    if (od_fan_fail) target_low = 20.0f * ref_mult;   
    if (id_fan_fail) target_high = 650.0f * ref_mult; 

    float estimated_low_sat = (target_low / ref_mult) * 0.3f + 10.0f; 
    sim_od_suction_temp = add_noise(estimated_low_sat + target_sh, 0.4f);  
    sim_od_liquid_temp = add_noise(set_id_temp + 12.0f + (extreme_ambient_penalty * 0.05f), 0.4f);  
    sim_od_discharge = add_noise(sim_od_suction_temp + (comp_ratio * 24.0f) + 55.0f + extreme_ambient_penalty + vol_eff_penalty, 2.0f);
    target_supply = (set_id_temp + 27.0f) + heat_boost + (extreme_ambient_penalty * 0.1f); 

    if (fault_comp_bypass) sim_od_suction_temp += 35.0f; 
    if (fault_rv_bypass) sim_od_suction_temp += 45.0f; 
    if (fault_non_condensables) sim_od_liquid_temp -= 12.0f; 
    if (fault_clogged_txv || fault_clogged_piston) sim_od_liquid_temp -= 15.0f; 

    if (id_fan_fail) sim_od_discharge = add_noise(250.0f, 5.0f);
    if (od_fan_fail) sim_od_suction_temp = add_noise(5.0f, 0.5f); 
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

  if (id_fan_fail) { if (heat_boost > 0) target_supply = 160.0f; else target_supply = set_id_temp; }
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

  if (is_compressor && !od_fan_fail && !is_defrosting) {
      sim_od_fan_amps = add_noise(1.8f, 0.05f);
  } else {
      sim_od_fan_amps = 0.0f;
  }

  bool id_fan_on = (state_g || state_y || state_w || fault_active[25]) && !id_fan_fail;
  if (id_fan_on) {
      float base_id_amps = 4.5f; 
      if (fault_low_id_cfm) base_id_amps = 3.6f; 
      if (fault_high_id_cfm) base_id_amps = 5.3f;
      sim_id_fan_amps = add_noise(base_id_amps, 0.08f);
  } else {
      sim_id_fan_amps = 0.0f;
  }

  sim_hs_amps = 0.0f;
  if (!hs1_broken) {
    if (hs1_t1_on)  sim_hs_amps += 10.8f;
    if (hs1_t23_on) sim_hs_amps += 10.8f;
  }
  if (!hs2_broken) {
    if (hs2_t1_on)  sim_hs_amps += 10.8f;
    if (hs2_t23_on) sim_hs_amps += 10.8f;
  }
  if (!hs3_broken) {
    if (hs3_on)     sim_hs_amps += 21.6f;
  }
  if (sim_hs_amps > 0.0f) {
      sim_hs_amps = add_noise(sim_hs_amps, 0.3f);
  }
}

void handle_hvac_logic() {
  uint32_t now = millis();
  bool current_w = readDebounced(PIN_W, state_w, debounce_w);
  bool current_y = readDebounced(PIN_Y, state_y, debounce_y);
  bool current_o = readDebounced(PIN_O, state_o, debounce_o);
  bool current_g = readDebounced(PIN_G, state_g, debounce_g); 

  bool send_update = false;
  if (current_w != last_w_state || current_y != last_y_state || current_o != last_o_state || current_g != last_g_state) { send_update = true; }

  // Keep thermostat telemetry alive even if relay boards are unavailable.
  if (!i2c_boards_present) {
    last_w_state = current_w;
    last_y_state = current_y;
    last_o_state = current_o;
    last_g_state = current_g;
    if (send_update) { notifyClients(); }
    return;
  }

  board_3.digitalWrite(6, (phys_lps_tripped || fault_active[7]) ? LOW : HIGH); 
  board_3.digitalWrite(7, (phys_hps_tripped || fault_active[8]) ? LOW : HIGH); 

  if (current_w && !last_w_state) {
    on_hs1_t1 = now + (esp_random() % 19001) + 1000; on_hs1_t23 = now + (esp_random() % 40001) + 20000;    
    on_hs2_t1 = now + (esp_random() % 19001) + 1000; on_hs2_t23 = now + (esp_random() % 40001) + 20000;    
    off_hs1_t1 = 0; off_hs1_t23 = 0; off_hs2_t1 = 0; off_hs2_t23 = 0; off_hs3 = 0; seq3_timer_started = false;
  }
  
  if (!current_w && last_w_state) {
    on_hs1_t1 = 0; on_hs1_t23 = 0; on_hs2_t1 = 0; on_hs2_t23 = 0; on_hs3 = 0; seq3_timer_started = false;
    if (hs1_t1_on)  off_hs1_t1 = now + (esp_random() % 70001) + 40000; if (hs1_t23_on) off_hs1_t23 = now + (esp_random() % 29001) + 1000;   
    if (hs2_t1_on)  off_hs2_t1 = now + (esp_random() % 70001) + 40000; if (hs2_t23_on) off_hs2_t23 = now + (esp_random() % 29001) + 1000;   
    if (hs3_on)     off_hs3 = now + (esp_random() % 29001) + 1000;       
  }

  if (on_hs1_t1 > 0 && now >= on_hs1_t1) { if (!sim_active[12]) board_2.digitalWrite(0, LOW); hs1_t1_on = true; on_hs1_t1 = 0; send_update = true; }
  if (on_hs1_t23 > 0 && now >= on_hs1_t23) { board_2.digitalWrite(1, LOW); board_2.digitalWrite(2, LOW); hs1_t23_on = true; on_hs1_t23 = 0; send_update = true; }
  if (on_hs2_t1 > 0 && now >= on_hs2_t1) { board_2.digitalWrite(3, LOW); hs2_t1_on = true; on_hs2_t1 = 0; send_update = true;
    if (!seq3_timer_started && current_w) { on_hs3 = now + (esp_random() % 60001) + 30000; seq3_timer_started = true; } }
  if (on_hs2_t23 > 0 && now >= on_hs2_t23) { board_2.digitalWrite(4, LOW); board_2.digitalWrite(5, LOW); hs2_t23_on = true; on_hs2_t23 = 0; send_update = true; }
  if (on_hs3 > 0 && now >= on_hs3) { board_2.digitalWrite(6, LOW); board_2.digitalWrite(7, LOW); hs3_on = true; on_hs3 = 0; send_update = true; }

  if (off_hs1_t1 > 0 && now >= off_hs1_t1) { if (!sim_active[11]) board_2.digitalWrite(0, HIGH); hs1_t1_on = false; off_hs1_t1 = 0; send_update = true; }
  if (off_hs1_t23 > 0 && now >= off_hs1_t23) { if (!sim_active[11]) { board_2.digitalWrite(1, HIGH); board_2.digitalWrite(2, HIGH); } hs1_t23_on = false; off_hs1_t23 = 0; send_update = true; }
  if (off_hs2_t1 > 0 && now >= off_hs2_t1) { board_2.digitalWrite(3, HIGH); hs2_t1_on = false; off_hs2_t1 = 0; send_update = true; }
  if (off_hs2_t23 > 0 && now >= off_hs2_t23) { board_2.digitalWrite(4, HIGH); board_2.digitalWrite(5, HIGH); hs2_t23_on = false; off_hs2_t23 = 0; send_update = true; }
  if (off_hs3 > 0 && now >= off_hs3) { board_2.digitalWrite(6, HIGH); board_2.digitalWrite(7, HIGH); hs3_on = false; off_hs3 = 0; send_update = true; }

  if (current_y && !last_y_state) { if (!sim_active[14]) { board_2.digitalWrite(12, LOW); board_2.digitalWrite(11, LOW); board_2.digitalWrite(9, LOW); if (!sim_active[15]) board_2.digitalWrite(8, LOW); } }
  if (!current_y && last_y_state) { if (!sim_active[14]) { board_2.digitalWrite(12, HIGH); board_2.digitalWrite(11, HIGH); } board_2.digitalWrite(9, HIGH); board_2.digitalWrite(8, HIGH); }
  
  if (current_o && !last_o_state) { if (!sim_active[13]) board_2.digitalWrite(10, LOW); }
  if (!current_o && last_o_state) { if (!sim_active[13]) board_2.digitalWrite(10, HIGH); }

  last_w_state = current_w; last_y_state = current_y; last_o_state = current_o; last_g_state = current_g;
  if (send_update) { notifyClients(); }
}

void handle_simulations() {
  uint32_t now = millis();
  bool current_w = state_w; bool current_y = state_y; bool current_o = state_o; 

  if (sim_active[1] && current_y && current_o) {
    if (sim_step[1] == 0) { board_1.digitalWrite(12, LOW); sim_timer[1] = now + 25000; sim_step[1] = 1; } 
    else if (sim_step[1] == 1 && now >= sim_timer[1]) { board_3.digitalWrite(6, LOW); sim_step[1] = 2; }
  } else if (sim_active[1] && (!current_y || !current_o)) { board_1.digitalWrite(12, HIGH); board_3.digitalWrite(6, HIGH); sim_step[1] = 0; }
  
  if (sim_active[2] && current_w) {
    if (sim_step[2] == 0) { board_1.digitalWrite(12, LOW); sim_timer[2] = now + 20000; sim_step[2] = 1; } 
    else if (sim_step[2] == 1 && now >= sim_timer[2]) { board_1.digitalWrite(1, LOW); board_1.digitalWrite(5, LOW); board_1.digitalWrite(9, LOW); sim_step[2] = 2; }
  } else if (sim_active[2] && !current_w) { board_1.digitalWrite(12, HIGH); board_1.digitalWrite(1, HIGH); board_1.digitalWrite(5, HIGH); board_1.digitalWrite(9, HIGH); sim_step[2] = 0; }
  
  if (sim_active[3] && current_y && current_o) {
    if (sim_step[3] == 0) { board_3.digitalWrite(5, LOW); sim_timer[3] = now + 30000; sim_step[3] = 1; } 
    else if (sim_step[3] == 1 && now >= sim_timer[3]) { board_3.digitalWrite(7, LOW); sim_step[3] = 2; }
  } else if (sim_active[3] && (!current_y || !current_o)) { board_3.digitalWrite(5, HIGH); board_3.digitalWrite(7, HIGH); sim_step[3] = 0; }
  
  if (sim_active[4] && current_y && !current_o) {
    if (sim_step[4] == 0) { board_3.digitalWrite(5, LOW); sim_timer[4] = now + 35000; sim_step[4] = 1; } 
    else if (sim_step[4] == 1 && now >= sim_timer[4]) { board_3.digitalWrite(6, LOW); sim_step[4] = 2; }
  } else if (sim_active[4] && (!current_y || current_o)) { board_3.digitalWrite(5, HIGH); board_3.digitalWrite(6, HIGH); sim_step[4] = 0; }
  
  if (sim_active[5]) { board_3.digitalWrite(11, LOW); }
  
  if (sim_active[6] && (current_y || current_w)) {
    if (now >= sim_timer[6]) {
      if (sim_step[6] == 0) { board_1.digitalWrite(12, LOW); sim_timer[6] = now + 5000; sim_step[6] = 1; } 
      else { board_1.digitalWrite(12, HIGH); sim_timer[6] = now + 15000; sim_step[6] = 0; }
    }
  } else if (sim_active[6]) { board_1.digitalWrite(12, HIGH); sim_step[6] = 0; sim_timer[6] = 0; }
  
  if (sim_active[7]) { if (current_w) { board_1.digitalWrite(2, LOW); board_1.digitalWrite(6, LOW); board_1.digitalWrite(10, LOW); } if (current_y) { board_3.digitalWrite(7, LOW); } }
  
  if (sim_active[8]) board_1.digitalWrite(3, LOW); 
  if (sim_active[9]) board_1.digitalWrite(7, LOW);  
  if (sim_active[10]) board_1.digitalWrite(11, LOW); 
  
  if (sim_active[11]) { board_2.digitalWrite(0, LOW); board_2.digitalWrite(1, LOW); board_2.digitalWrite(2, LOW); }
  if (sim_active[12]) { board_2.digitalWrite(0, HIGH); }
  
  if (sim_active[13] && current_o) {
    if (now >= sim_timer[13]) {
      if (sim_step[13] == 0) { board_3.digitalWrite(11, LOW); sim_timer[13] = now + (esp_random() % 3000 + 1000); sim_step[13] = 1; } 
      else { board_3.digitalWrite(11, HIGH); sim_timer[13] = now + (esp_random() % 8000 + 4000); sim_step[13] = 0; }
    }
  } else if (sim_active[13] && !current_o) { board_3.digitalWrite(11, HIGH); sim_step[13] = 0; sim_timer[13] = 0; }
  
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

void runCommsSlice() {
  bool wifi_connected = (WiFi.status() == WL_CONNECTED);
  if (wifi_connected && !wifi_was_connected) {
    wifi_connected_flash_until = millis() + WIFI_CONNECTED_FLASH_MS;
    wifi_disconnected_since = 0;
  } else if (!wifi_connected && wifi_was_connected && wifi_disconnected_since == 0) {
    wifi_disconnected_since = millis();
  }
  wifi_was_connected = wifi_connected;

  if (is_ap_mode || setup_ap_active) { dnsServer.processNextRequest(); }
  else if (wifi_ssid.length() > 0 && WiFi.status() != WL_CONNECTED) {
    uint32_t now = millis();
    if (wifi_disconnected_since == 0) {
      wifi_disconnected_since = now;
    }

    if ((now - wifi_disconnected_since) >= WIFI_AP_FAILOVER_MS) {
      Serial.println("WiFi reconnect timeout. Switching to AP mode.");
      is_ap_mode = true;
      WiFi.mode(WIFI_AP);
      setup_ap_active = WiFi.softAP("Vesta Core Trainer", "8037945526");
      dnsServer.start(53, "*", WiFi.softAPIP());
      wifi_reconnect_timer = now;
      wifi_disconnected_since = now;
      return;
    }

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
  // FIXED BLE RECONNECT ENGINE
  // =============================================================
  if (!deviceConnected && oldDeviceConnected) {
    vTaskDelay(pdMS_TO_TICKS(200)); // Small cooldown to clear old RF connection frames
    NimBLEDevice::getAdvertising()->stop(); // Clear internal driver flag trace
    vTaskDelay(pdMS_TO_TICKS(50));
    NimBLEDevice::getAdvertising()->start(); // Re-announce presence back to the smartphone
    Serial.println("BLE Disconnect Handled: Restarting Advertising Beacon...");
    oldDeviceConnected = deviceConnected;
  }
  
  if (deviceConnected && !oldDeviceConnected) {
    Serial.println("Smartphone Connection Verified via BLE.");
    oldDeviceConnected = deviceConnected;
  }

  if (force_telemetry_update) {
    force_telemetry_update = false;
    notifyClients(true);
  }

  sendEngineHeartbeat();
  handle_system_health();
}

void syncFurnaceTelemetryToUnifiedState() {
  sim_comp_amps = furnace_physics.getCompAmps();
  sim_od_fan_amps = furnace_physics.getOdFanAmps();
  sim_id_fan_amps = furnace_physics.getIdFanAmps();
  sim_hs_amps = 0.0f;

  phys_lps_tripped = furnace_physics.isLpsTripped();
  phys_hps_tripped = furnace_physics.isHpsTripped();

  sim_od_low_press = furnace_physics.getOdLowPress();
  sim_od_high_press = furnace_physics.getOdHighPress();
  sim_od_liquid_press = furnace_physics.getOdLiquidPress();
  sim_od_suction_temp = furnace_physics.getOdSuctionTemp();
  sim_od_liquid_temp = furnace_physics.getOdLiquidTemp();
  sim_od_discharge = furnace_physics.getOdDischargeTemp();
  sim_od_ambient = furnace_physics.getOdAmbient();

  sim_id_ambient = furnace_physics.getIdAmbient();
  sim_id_return_temp = furnace_physics.getIdReturnTemp();
  sim_id_supply_temp = furnace_physics.getIdSupplyTemp();
  sim_id_rh = furnace_physics.getIdRh();
}

void runHeatPumpControlSlice() {
  handle_hvac_logic();
  handle_telemetry();
  if (i2c_boards_present) {
    handle_simulations();
  }
}

void runFurnaceControlSlice() {
  bool current_w = readDebounced(PIN_W, state_w, debounce_w);
  bool current_y = readDebounced(PIN_Y, state_y, debounce_y);
  bool current_g = readDebounced(PIN_G, state_g, debounce_g);
  state_o = false;

  bool send_update = (current_w != last_w_state) ||
                     (current_y != last_y_state) ||
                     (current_g != last_g_state);

  bool gas_valve_active = readOptoActiveLow(PIN_GAS_VALVE_MONITOR);
  bool blower_running = readOptoActiveLow(PIN_BLOWER_MONITOR);

  furnace_controller.update(current_w, furnace_physics.getIdSupplyTemp());
  furnace_physics.setAmbient(set_od_temp, set_id_temp, set_rh);
  furnace_physics.setRefrigerant(current_refrigerant, id_is_txv);
  furnace_physics.update(current_y, gas_valve_active, current_g, blower_running, fault_active);
  syncFurnaceTelemetryToUnifiedState();

  if (i2c_boards_present) {
    board_3.digitalWrite(6, (phys_lps_tripped || fault_active[7] || fault_active[26]) ? LOW : HIGH);
    board_3.digitalWrite(7, (phys_hps_tripped || fault_active[8] || fault_active[27]) ? LOW : HIGH);
  }

  last_w_state = current_w;
  last_y_state = current_y;
  last_g_state = current_g;

  if (send_update) {
    notifyClients();
  }
}

void runControlSlice() {
  switch (active_trainer_type) {
    case STRAIGHT_AC_FURNACE:
      runFurnaceControlSlice();
      break;
    case HEAT_PUMP:
    default:
      runHeatPumpControlSlice();
      break;
  }
}

void commTask(void* parameter) {
  (void)parameter;
  for (;;) {
    runCommsSlice();
    vTaskDelay(pdMS_TO_TICKS(COMM_TASK_SLICE_MS));
  }
}

void controlTask(void* parameter) {
  (void)parameter;
  for (;;) {
    runControlSlice();
    vTaskDelay(pdMS_TO_TICKS(CONTROL_TASK_SLICE_MS));
  }
}

void loop() {
  if (!dual_core_runtime_enabled) {
    runCommsSlice();
    runControlSlice();
    return;
  }

  vTaskDelay(pdMS_TO_TICKS(1000));

}

void connectToMqtt() {
  Serial.println("Connecting to MQTT...");
  mqttClient.connect();
}
