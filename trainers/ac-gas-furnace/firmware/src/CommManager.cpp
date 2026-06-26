#include "CommManager.h"
#include "PhysicsEngine.h"
#include "FurnaceController.h"
#include "main.h"
#include <WiFi.h>
#include <esp_mac.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <DNSServer.h>
#include <Adafruit_NeoPixel.h>
#include <Update.h>

// --- Global variables from main.cpp that CommManager needs to access ---
extern bool sim_active[16];
extern int sim_step[16];
extern uint32_t sim_timer[16];
extern String latest_diagnosis;
extern int student_score;
extern String work_history_log;
extern String current_refrigerant;
extern volatile bool force_telemetry_update;
extern String authToken;
extern String authRole;
extern uint32_t authExpiry;
extern const uint32_t AUTH_TOKEN_TTL;
extern PCF8575 board_1;
extern PCF8575 board_2;
extern PCF8575 board_3;
extern bool is_ap_mode;
extern bool last_w_state, last_y_state, last_g_state;
extern String wifi_ssid;
extern String wifi_pass;
extern uint32_t reset_counter;
extern String ble_login_status;

// --- Forward declarations for functions in main.cpp ---
void reset_all_faults_and_sims();
void handleDiagnosis(String submitted);
String generateAuthToken();
bool isAuthenticatedRequest(AsyncWebServerRequest *request);
bool isAdminRequest(AsyncWebServerRequest *request);
void logLogin(String username, String role);
bool checkCredentials(String user, String pass);
String getUserRole(String user);


CommManager::CommManager(PhysicsEngine& physics, FurnaceController& furnace)
    : physics_engine(physics), furnace_controller(furnace), server(80), ws("/ws"),
      pServer(nullptr), pTxCharacteristic(nullptr), deviceConnected(false), oldDeviceConnected(false),
      negotiated_mtu(20), ble_timer(0), ble_restart_state(BLE_IDLE), ble_restart_timer(0) {}

void CommManager::begin() {
    setupBLE();
    setupWebServer();
    server.begin();
}

void CommManager::loop() {
    ws.cleanupClients();

    if (deviceConnected && !oldDeviceConnected) {
        Serial.println("Smartphone Connection Verified via BLE.");
        oldDeviceConnected = deviceConnected;
        ble_restart_state = BLE_IDLE;
    } else if (!deviceConnected && oldDeviceConnected) {
        Serial.println("BLE device disconnected. Starting non-blocking restart of advertising...");
        oldDeviceConnected = false;
        ble_restart_state = BLE_DISCONNECTED;
        ble_restart_timer = millis();
    }

    if (ble_restart_state == BLE_DISCONNECTED && millis() - ble_restart_timer > 200) {
        NimBLEDevice::getAdvertising()->stop();
        Serial.println("BLE Advertising stopped.");
        ble_restart_state = BLE_RESTARTING;
        ble_restart_timer = millis();
    }
    
    if (ble_restart_state == BLE_RESTARTING && millis() - ble_restart_timer > 50) {
        NimBLEDevice::getAdvertising()->start();
        Serial.println("BLE Advertising restarted.");
        ble_restart_state = BLE_IDLE;
    }
}

String CommManager::getStatusJSON() {
    uint32_t uptime_sec = millis() / 1000;
    char uptime_str[15];
    snprintf(uptime_str, sizeof(uptime_str), "%02d:%02d:%02d", (int)(uptime_sec / 3600), (int)((uptime_sec % 3600) / 60), (int)(uptime_sec % 60));

    float low_noise = (esp_random() % 100) / 100.0 * 2.0 - 1.0;
    float high_noise = (esp_random() % 100) / 100.0 * 4.0 - 2.0;
    float liquid_noise = (esp_random() % 100) / 100.0 * 3.0 - 1.5;

    DynamicJsonDocument doc(8192); // Increased size to hold all fault states
    
    doc["w_call"] = last_w_state;
    doc["y_call"] = last_y_state;
    doc["g_call"] = last_g_state;
    
    doc["wifi_rssi"] = is_ap_mode ? 0 : WiFi.RSSI();
    doc["ram"] = ESP.getFreeHeap();
    doc["uptime"] = uptime_str;
    doc["clients"] = ws.count();
    doc["diagnosis"] = latest_diagnosis;
    doc["student_score"] = student_score;
    doc["work_history"] = work_history_log;
    doc["refrigerant"] = current_refrigerant;
    doc["reset_counter"] = reset_counter;
    doc["ble_login_status"] = ble_login_status;

    extern const char* TRAINER_TYPE;
    doc["trainer_type"] = TRAINER_TYPE;

    doc["phys_lps"] = physics_engine.isLpsTripped() ? 1 : 0;
    doc["phys_hps"] = physics_engine.isHpsTripped() ? 1 : 0;
    doc["lps_open"] = (physics_engine.isLpsTripped() || fault_active[7] || fault_active[26]) ? 1 : 0;
    doc["hps_open"] = (physics_engine.isHpsTripped() || fault_active[8] || fault_active[27]) ? 1 : 0;

    doc["od_low_press"] = round((physics_engine.getOdLowPress() + low_noise) * 10.0) / 10.0;
    doc["od_high_press"] = round((physics_engine.getOdHighPress() + high_noise) * 10.0) / 10.0;
    doc["od_liquid_press"] = round((physics_engine.getOdLiquidPress() + liquid_noise) * 10.0) / 10.0;
    doc["od_suction_temp"] = round(physics_engine.getOdSuctionTemp() * 10.0) / 10.0;
    doc["od_liquid_temp"] = round(physics_engine.getOdLiquidTemp() * 10.0) / 10.0;
    doc["od_ambient"] = round(physics_engine.getOdAmbient() * 10.0) / 10.0;

    doc["id_return_temp"] = round(physics_engine.getIdReturnTemp() * 10.0) / 10.0;
    doc["id_supply_temp"] = round(physics_engine.getIdSupplyTemp() * 10.0) / 10.0;
    doc["id_ambient"] = round(physics_engine.getIdAmbient() * 10.0) / 10.0;
    doc["id_rh"] = round(physics_engine.getIdRh() * 10.0) / 10.0;
    doc["id_suction_temp"] = round((physics_engine.getOdSuctionTemp() + 1.2) * 10.0) / 10.0;
    doc["id_liquid_temp"] = round((physics_engine.getOdLiquidTemp() - 1.2) * 10.0) / 10.0;

    doc["comp_amps"] = round(physics_engine.getCompAmps() * 10.0) / 10.0;
    doc["od_fan_amps"] = round(physics_engine.getOdFanAmps() * 10.0) / 10.0;
    doc["id_fan_amps"] = round(physics_engine.getIdFanAmps() * 10.0) / 10.0;
    doc["hs_amps"] = round(furnace_controller.isHeatBlowerOn() ? (furnace_controller.isInducerOn() ? 1.2 : 0) + (furnace_controller.isIgniterOn() ? 3.5 : 0) + (furnace_controller.isGasValveOn() ? 0.5 : 0) : 0.0);

    doc["furnace_state"] = furnace_controller.getFurnaceState();
    doc["inducer_on"] = furnace_controller.isInducerOn() ? 1 : 0;
    doc["igniter_on"] = furnace_controller.isIgniterOn() ? 1 : 0;
    doc["gas_valve_on"] = furnace_controller.isGasValveOn() ? 1 : 0;
    doc["heat_blower_on"] = furnace_controller.isHeatBlowerOn() ? 1 : 0;

    // Add new PhysicsEngine telemetry
    doc["sim_cfm"] = physics_engine.getSimulatedCfm();
    doc["sim_static_pressure"] = physics_engine.getStaticPressure();
    doc["telemetry_state"] = physics_engine.getTelemetryState();
    doc["sim_high_limit_tripped"] = physics_engine.isHighLimitTripped();
    doc["sim_flame_active"] = physics_engine.isFlameActive();
    doc["sim_blower_running"] = physics_engine.isBlowerRunning();
    
    // Export full fault/simulation bitfields
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

    // Relay state exports
    doc["relay_inducer"] = furnace_controller.isInducerOn() ? 1 : 0;
    doc["relay_igniter"] = furnace_controller.isIgniterOn() ? 1 : 0;
    doc["relay_gas_valve"] = furnace_controller.isGasValveOn() ? 1 : 0;
    doc["relay_heat_blower"] = furnace_controller.isHeatBlowerOn() ? 1 : 0;
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

    if (ble_login_status == "success" && authToken.length() > 0) {
        doc["auth_token"] = authToken;
    }

    String json;
    serializeJson(doc, json);
    return json;
}

void CommManager::notifyClients(bool high_priority) {
    String json = getStatusJSON();
    bool delivered = false;

    if (ws.count() > 0) {
        ws.textAll(json);
        delivered = true;
    }

    uint32_t now = millis();
    if (deviceConnected && pTxCharacteristic && (high_priority || (now - ble_timer >= 1500))) {
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

    if (delivered && ble_login_status != "None") {
        ble_login_status = "None";
    }
}

// --- BLE Callback Classes (defined locally) ---
class MyServerCallbacks : public NimBLEServerCallbacks {
    CommManager* m_pCommManager;
public:
    MyServerCallbacks(CommManager* pCommManager) : m_pCommManager(pCommManager) {}
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
        m_pCommManager->deviceConnected = true;
        m_pCommManager->negotiated_mtu = connInfo.getMTU() - 3;
        if (m_pCommManager->negotiated_mtu < 20) m_pCommManager->negotiated_mtu = 20;
    }
    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
        m_pCommManager->deviceConnected = false;
        m_pCommManager->negotiated_mtu = 20;
    }
    void onMTUChange(uint16_t MTU, NimBLEConnInfo& connInfo) override {
        m_pCommManager->negotiated_mtu = MTU - 3;
        if (m_pCommManager->negotiated_mtu < 20) m_pCommManager->negotiated_mtu = 20;
    }
};

class MyCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo& connInfo) override {
        std::string rxValue = pCharacteristic->getValue();
        if (rxValue.length() > 0) {
            Serial.printf("BLE Received Payload: %s\n", rxValue.c_str());
            DynamicJsonDocument doc(2048);
            DeserializationError error = deserializeJson(doc, rxValue.c_str());
            if (!error) {
                if (!doc["reset_score"].isNull()) {
                    student_score = 100;
                    work_history_log = "";
                    latest_diagnosis = "None";
                }
                if (!doc["diagnosis"].isNull()) {
                    handleDiagnosis(doc["diagnosis"].as<String>());
                }
                if (!doc["user"].isNull() && !doc["pass"].isNull()) {
                    String u = doc["user"].as<String>();
                    String p = doc["pass"].as<String>();
                    if (checkCredentials(u, p)) {
                        authToken = generateAuthToken();
                        authRole = getUserRole(u);
                        authExpiry = (millis() / 1000) + AUTH_TOKEN_TTL;
                        ble_login_status = "success";
                        logLogin(u, authRole);
                    } else {
                        ble_login_status = "denied";
                    }
                    force_telemetry_update = true;
                }
                force_telemetry_update = true;
            }
        }
    }
};

void CommManager::setupBLE() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char bleName[25];
    sprintf(bleName, "VestaCore%02X%02X", mac[4], mac[5]);

    NimBLEDevice::init(bleName);
    NimBLEDevice::setMTU(512);
    pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks(this));

    NimBLEService *pService = pServer->createService("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
    pTxCharacteristic = pService->createCharacteristic("6E400003-B5A3-F393-E0A9-E50E24DCCA9E", NIMBLE_PROPERTY::NOTIFY);
    NimBLECharacteristic *pRxCharacteristic = pService->createCharacteristic("6E400002-B5A3-F393-E0A9-E50E24DCCA9E", NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    pRxCharacteristic->setCallbacks(new MyCallbacks());

    NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->addServiceUUID("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
    pAdvertising->enableScanResponse(true);
    pAdvertising->start();
}

void CommManager::setupWebServer() {
    extern DNSServer dnsServer;
    ws.onEvent([this](AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
        if (type == WS_EVT_CONNECT) {
            client->text(this->getStatusJSON());
        }
    });
    server.addHandler(&ws);

    server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(200, "text/html", "<form method='POST' action='/update' enctype='multipart/form-data'><input type='file' name='update'><input type='submit' value='Update'></form>");
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
        extern bool ota_in_progress;
        extern Adafruit_NeoPixel status_led;
        if (!index) {
            ota_in_progress = true;
            status_led.setPixelColor(0, status_led.Color(0, 0, 255)); status_led.show();
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { Update.printError(Serial); }
        }
        if (len) { if (Update.write(data, len) != len) { Update.printError(Serial); } }
        if (final) {
            ota_in_progress = false;
            if (Update.end(false)) {
                status_led.setPixelColor(0, status_led.Color(0, 255, 0)); status_led.show();
            } else {
                status_led.setPixelColor(0, status_led.Color(255, 0, 0)); status_led.show();
                Update.printError(Serial);
            }
        }
    });

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncWebServerResponse *response = request->beginResponse(LittleFS, "/index.html", "text/html");
        response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
        request->send(response);
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

    server.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest *request){ 
        request->send(200, "application/json", this->getStatusJSON()); 
    });

    server.on("/api/reset", HTTP_POST, [](AsyncWebServerRequest *request){ 
        if(!isAdminRequest(request)) { request->send(401, "text/plain", "DENIED"); return; } 
        reset_all_faults_and_sims(); 
        force_telemetry_update = true; 
        request->send(200, "text/plain", "OK"); 
    });

    server.on("/api/submit", HTTP_POST, [](AsyncWebServerRequest *request){
        if (!isAuthenticatedRequest(request)) { request->send(401, "text/plain", "DENIED"); return; }
        if (request->hasParam("diagnosis")) { 
            handleDiagnosis(request->getParam("diagnosis")->value());
            if (latest_diagnosis.startsWith("CORRECT")) {
                request->send(200, "text/plain", "CORRECT");
            } else {
                request->send(200, "text/plain", "INCORRECT");
            }
        } else { request->send(400, "text/plain", "Bad Request"); }
    });

    server.on("/api/ambient", HTTP_POST, [this](AsyncWebServerRequest *request){
        if (!isAuthenticatedRequest(request)) { request->send(401, "text/plain", "DENIED"); return; }
        if (request->hasParam("od", true) && request->hasParam("id", true)) {
            float od = request->getParam("od", true)->value().toFloat();
            float id = request->getParam("id", true)->value().toFloat();
            float rh = request->hasParam("rh", true) ? request->getParam("rh", true)->value().toFloat() : 50.0;
            this->physics_engine.setAmbient(od, id, rh);
            force_telemetry_update = true; 
            request->send(200, "text/plain", "OK");
        } else { request->send(400, "text/plain", "Bad Request"); }
    });

    server.on("/api/refrigerant", HTTP_POST, [this](AsyncWebServerRequest *request){
        if (!isAuthenticatedRequest(request)) { request->send(401, "text/plain", "DENIED"); return; }
        if (request->hasParam("type")) {
            current_refrigerant = request->getParam("type")->value();
            this->physics_engine.setRefrigerant(current_refrigerant, true);
            force_telemetry_update = true; 
            request->send(200, "text/plain", "OK");
        } else { request->send(400, "text/plain", "Bad Request"); }
    });

    server.on("/api/metering", HTTP_POST, [this](AsyncWebServerRequest *request){
        if (!isAuthenticatedRequest(request)) { request->send(401, "text/plain", "DENIED"); return; }
        bool is_txv = request->hasParam("id_txv") ? (request->getParam("id_txv")->value() == "1") : true;
        this->physics_engine.setRefrigerant(current_refrigerant, is_txv);
        force_telemetry_update = true; 
        request->send(200, "text/plain", "OK");
    });

    server.on("/api/toggle", HTTP_POST, [](AsyncWebServerRequest *request){
        if(!isAdminRequest(request)) { request->send(401, "text/plain", "DENIED"); return; }
        if (request->hasParam("id") && request->hasParam("state")) {
            String id = request->getParam("id")->value();
            bool state = request->getParam("state")->value().toInt() == 1;
            int val_inv = state ? LOW : HIGH;

            if (id == "reset_score") {
                student_score = 100;
                work_history_log = "";
                latest_diagnosis = "None";
            } else if (id.startsWith("sim_")) {
                int simNum = id.substring(4).toInt();
                if (simNum >= 1 && simNum <= 15) { 
                    sim_active[simNum] = state; 
                    sim_timer[simNum] = 0; 
                    sim_step[simNum] = 0; 
                }
            } else if (id.startsWith("f")) {
                int f = id.substring(1).toInt();
                if (f >= 0 && f < 55) fault_active[f] = state;
            }
            force_telemetry_update = true;
            request->send(200, "text/plain", "OK");
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
            DynamicJsonDocument doc(512);
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
            request->send(200, "text/html", "<html><body style='font-family:sans-serif;'><h2>Saved! Reconnecting to Wi-Fi now.</h2></body></html>");
        } else { request->send(400, "text/plain", "Missing credentials"); }
    });

    server.on("/api/users/add", HTTP_POST, [](AsyncWebServerRequest *request){
        if (!isAdminRequest(request)) { request->send(401, "text/plain", "DENIED"); return; }
        if (request->hasParam("user", true) && request->hasParam("pass", true) && request->hasParam("role", true)) {
            String newUser = request->getParam("user", true)->value();
            String newPass = request->getParam("pass", true)->value();
            String newRole = request->getParam("role", true)->value();

            newUser.trim(); newPass.trim(); newRole.trim();
            newUser.toLowerCase(); newRole.toLowerCase();

            if (newUser.length() == 0 || newPass.length() == 0) {
                request->send(400, "text/plain", "Bad Request"); return;
            }

            File f = LittleFS.open("/users.json", FILE_READ);
            DynamicJsonDocument db(2048); deserializeJson(db, f); f.close();
            db[newUser]["pw"] = newPass;
            db[newUser]["role"] = newRole;
            f = LittleFS.open("/users.json", FILE_WRITE); serializeJson(db, f); f.close();
            logLogin("SYSTEM", "Created/Updated Profile: " + newUser);
            request->send(200, "text/plain", "User Encoded");
        }
    }, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        if (!isAdminRequest(request)) { request->send(401, "text/plain", "DENIED"); return; }
        DynamicJsonDocument doc(len + 512);
        if (deserializeJson(doc, data, len) == DeserializationError::Ok && !doc["user"].isNull() && !doc["pass"].isNull()) {
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
    
    server.onNotFound([](AsyncWebServerRequest *request){
        if (request->method() == HTTP_OPTIONS) {
            request->send(200);
        } else if (is_ap_mode) { 
            request->redirect("http://" + WiFi.softAPIP().toString() + "/"); 
        } else { 
            request->send(404, "text/plain", "Not Found"); 
        }
    });

    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "*");
}