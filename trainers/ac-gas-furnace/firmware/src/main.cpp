#include <Arduino.h>
#include <WiFi.h>
#include "main.h"
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
#include "PhysicsEngine.h"
#include "CommManager.h"
#include "FurnaceController.h"

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

// ==========================================
// GLOBAL TIMERS & STATES
// ==========================================
bool last_w_state = false; bool last_y_state = false; bool last_g_state = false;

uint32_t debounce_w = 0, debounce_y = 0, debounce_g = 0;
bool state_w = false, state_y = false, state_g = false;

// --- DYNAMIC ELECTRICAL TELEMETRY ---
bool i2c_boards_present = false;
FurnaceController furnace(board_2, i2c_boards_present);
PhysicsEngine physics;
CommManager comms(physics, furnace);

bool sim_active[16] = {false};
bool fault_active[55] = {false}; 

int sim_step[16] = {0};         
int limit_trip_count = 0;
int ignition_retry_count = 0;

// --- DYNAMIC REFRIGERANT PHYSICS ---
String current_refrigerant = "R410A";
uint32_t sim_timer[16] = {0};   

int active_scenario = 0;
uint32_t scenario_start_time = 0;
String latest_diagnosis = "None";
int student_score = 100;
String work_history_log = "";
uint32_t reset_counter = 0;
int total_score_sum = 0;
int completed_problems = 0;
int current_problem_score = 100;
String ble_login_status = "None";
String wifi_ssid = "ComfortSC";
String wifi_pass = "8037945526";
String authToken = "";
String authRole = "";
uint32_t authExpiry = 0;
extern const uint32_t AUTH_TOKEN_TTL = 28800;
volatile bool force_telemetry_update = false;

bool pending_reboot = false;
uint32_t reboot_timer = 0;

// --- DOCKER ENGINE LINK ---
const char* ENGINE_CONFIG_FILE = "/engine.json";
String engine_base_url = "";
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
void sendEngineHeartbeat();

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
  DynamicJsonDocument doc(256);
  DeserializationError error = deserializeJson(doc, f);
  f.close();
  if (error) return String();
  return doc["base_url"].as<String>();
}

void saveEngineBaseUrl(const String& baseUrl) {
  if (baseUrl.length() == 0) return;
  File f = LittleFS.open(ENGINE_CONFIG_FILE, FILE_WRITE);
  if (!f) return;
  DynamicJsonDocument doc(256);
  doc["base_url"] = baseUrl;
  serializeJson(doc, f);
  f.close();
}

String discoverEngineBaseUrl() {
  String stored = loadEngineBaseUrl();
  if (stored.length() > 0) return stored;

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
        Serial.printf("Engine found via scan: %s\n", scanBase);
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

  if (engine_base_url.length() == 0) {
    engine_base_url = discoverEngineBaseUrl();
  }

  uint32_t now = millis();
  bool changed = (state_w != hb_last_w) ||
                 (state_y != hb_last_y) ||
                 (state_g != hb_last_g) ||
                 (physics.isLpsTripped() != hb_last_lps) ||
                 (physics.isHpsTripped() != hb_last_hps);

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
  doc["phys_lps"] = physics.isLpsTripped();
  doc["phys_hps"] = physics.isHpsTripped();
  doc["trainer_type"] = TRAINER_TYPE;
  doc["wifi_rssi"] = WiFi.RSSI();
  doc["ram"] = ESP.getFreeHeap();
  doc["uptime"] = uptime_str;
  doc["temp"] = 0.0;

  String payload;
  serializeJson(doc, payload);

  if (postEngineHeartbeat(engine_base_url, payload)) {
    saveEngineBaseUrl(engine_base_url);
    hb_last_w = state_w;
    hb_last_y = state_y;
    hb_last_g = state_g;
    hb_last_lps = physics.isLpsTripped();
    hb_last_hps = physics.isHpsTripped();
    return;
  }

  String gatewayBase = "http://" + WiFi.gatewayIP().toString() + ":8000";
  if (gatewayBase != engine_base_url && postEngineHeartbeat(gatewayBase, payload)) {
    engine_base_url = gatewayBase;
    saveEngineBaseUrl(engine_base_url);
    hb_last_w = state_w;
    hb_last_y = state_y;
    hb_last_g = state_g;
    hb_last_lps = physics.isLpsTripped();
    hb_last_hps = physics.isHpsTripped();
    Serial.printf("Engine heartbeat switched to gateway endpoint: %s\n", engine_base_url.c_str());
    return;
  }

  String discoveredBase = discoverEngineBaseUrl();
  if (discoveredBase.length() > 0 && discoveredBase != engine_base_url && postEngineHeartbeat(discoveredBase, payload)) {
    engine_base_url = discoveredBase;
    saveEngineBaseUrl(engine_base_url);
    hb_last_w = state_w;
    hb_last_y = state_y;
    hb_last_g = state_g;
    hb_last_lps = physics.isLpsTripped();
    hb_last_hps = physics.isHpsTripped();
    Serial.printf("Engine heartbeat switched to discovered endpoint: %s\n", engine_base_url.c_str());
    return;
  }

  Serial.printf("Engine heartbeat failed at %s\n", engine_base_url.c_str());
}

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

// ==========================================
// BLE COMMUNICATION
// ==========================================
#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

bool checkCredentials(String user, String pass) { // Now used by CommManager
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

String getUserRole(String user) { // Now used by CommManager
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

void reset_all_faults_and_sims() {
  active_scenario = 0;
  scenario_start_time = 0;

  physics.reset();
  
  for(int i = 0; i < 16; i++) { sim_active[i] = false; sim_timer[i] = 0; sim_step[i] = 0; }
  for(int i = 0; i < 55; i++) { fault_active[i] = false; } 
  
  limit_trip_count = 0;

  furnace.reset();

  if (i2c_boards_present) {
    for (int i = 0; i < 16; i++) {
      board_1.digitalWrite(i, HIGH);
      board_2.digitalWrite(i, HIGH);
      board_3.digitalWrite(i, HIGH);
      yield(); // Prevent watchdog starvation during 48 blocking I2C writes
    }
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

void logLogin(String username, String role); // Forward declaration

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

  if(!LittleFS.begin(true)) { Serial.println("LittleFS Mount Failed"); return; }
  initUserDatabase();

  loadWiFiConfig();
  if (wifi_ssid.length() == 0) {
    wifi_ssid = "ComfortSC";
    wifi_pass = "8037945526";
  }

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);

  Serial.printf("WiFi attempting SSID: '%s'\n", wifi_ssid.c_str());
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
    Serial.println("\nWiFi connected.");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    engine_base_url = discoverEngineBaseUrl();
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

  // Start BLE/web comms after the network stack is configured.
  comms.begin();
  
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
  detectI2CBoards();
  if (i2c_boards_present) {
    board_1.begin();
    board_2.begin();
    board_3.begin();
    physics.begin();
    furnace.begin();
  }
  reset_all_faults_and_sims();

  AsyncWebServer server(80); // This is now a local variable for the routes left in main
  // NOTE: Some complex routes like user management are left here for simplicity.
  // They can be moved into CommManager in a future step if desired.
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
  
  if (sim_active[3] && current_y) { // Failed Condenser Fan
    if (sim_step[3] == 0) { board_3.digitalWrite(5, LOW); sim_timer[3] = now + 30000; sim_step[3] = 1; } 
    else if (sim_step[3] == 1 && now >= sim_timer[3]) { board_3.digitalWrite(7, LOW); sim_step[3] = 2; }
  } else if (sim_active[3] && !current_y) { board_3.digitalWrite(5, HIGH); board_3.digitalWrite(7, HIGH); sim_step[3] = 0; }
  
  if (sim_active[6] && (current_y || current_w)) {
    if (now >= sim_timer[6]) {
      if (sim_step[6] == 0) { board_1.digitalWrite(12, LOW); sim_timer[6] = now + 5000; sim_step[6] = 1; } 
      else { board_1.digitalWrite(12, HIGH); sim_timer[6] = now + 15000; sim_step[6] = 0; }
    }
  } else if (sim_active[6]) { board_1.digitalWrite(12, HIGH); sim_step[6] = 0; sim_timer[6] = 0; }
  
  if (sim_active[5] && furnace.getFurnaceState() == 4 /* FURNACE_HEATING */) { // Intermittent Flame Sensor
    if (now >= sim_timer[5]) {
      if (sim_step[5] == 0) { fault_active[32] = true; board_1.digitalWrite(3, LOW); sim_timer[5] = now + 2000; sim_step[5] = 1; } // Drop flame for 2s
      else { fault_active[32] = false; board_1.digitalWrite(3, HIGH); sim_timer[5] = now + 25000; sim_step[5] = 0; } // Flame good for 25s
    }
  } else if (sim_active[5]) { fault_active[32] = false; board_1.digitalWrite(3, HIGH); sim_step[5] = 0; sim_timer[5] = 0; }

  if (sim_active[8] && furnace.getFurnaceState() == 1 /* FURNACE_PRE_PURGE */) { // Fluttering Pressure Switch
    if (now >= sim_timer[8]) {
      if (sim_step[8] % 2 == 0) { fault_active[16] = true; board_1.digitalWrite(4, LOW); }
      else { fault_active[16] = false; board_1.digitalWrite(4, HIGH); }
      sim_timer[8] = now + (esp_random() % 800 + 200);
      sim_step[8]++;
    }
  } else if (sim_active[8]) { fault_active[16] = false; board_1.digitalWrite(4, HIGH); sim_step[8] = 0; sim_timer[8] = 0; }

  if (sim_active[9] && furnace.getFurnaceState() == 5 /* FURNACE_POST_PURGE */) { // Leaky Gas Valve
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
  
  if (pending_reboot && millis() > reboot_timer) {
    ESP.restart();
  }

  // =============================================================
  // TELEMETRY & EVENT DISPATCH ENGINE
  // =============================================================
  if (force_telemetry_update) { 
    comms.notifyClients(true); 
    force_telemetry_update = false; 
  }

  comms.loop();

  // Read thermostat inputs
  bool current_w = readDebounced(PIN_W, state_w, debounce_w);
  bool current_y = readDebounced(PIN_Y, state_y, debounce_y);
  bool current_g = readDebounced(PIN_G, state_g, debounce_g);

  bool thermostat_changed = (current_w != last_w_state || current_y != last_y_state || current_g != last_g_state);
  if (thermostat_changed) {
    comms.notifyClients();
  }

  last_w_state = current_w; last_y_state = current_y; last_g_state = current_g;

  if (i2c_boards_present) {
    furnace.update(current_w, physics.getIdSupplyTemp());
  }

  physics.update(current_y, current_w, current_g, furnace.isHeatBlowerOn(), fault_active);

  if (i2c_boards_present) {
    handle_simulations();
  }
  sendEngineHeartbeat();
  handle_system_health();

}
