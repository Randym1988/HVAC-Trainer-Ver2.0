#ifndef COMM_MANAGER_H
#define COMM_MANAGER_H

#include <ESPAsyncWebServer.h>
#include <NimBLEDevice.h>

class PhysicsEngine;
class FurnaceController;

class CommManager {
public:
    CommManager(PhysicsEngine& physics, FurnaceController& furnace);

    void begin();
    void loop();
    void notifyClients(bool high_priority = false);

private:
    friend class MyServerCallbacks;

    // --- Member References ---
    PhysicsEngine& physics_engine;
    FurnaceController& furnace_controller;

    // --- Web Server & Sockets ---
    AsyncWebServer server;
    AsyncWebSocket ws;
    String getStatusJSON();
    void setupWebServer();

    // --- BLE ---
    NimBLEServer* pServer;
    NimBLECharacteristic* pTxCharacteristic;
    volatile bool deviceConnected;
    volatile bool oldDeviceConnected;
    volatile int negotiated_mtu;
    uint32_t ble_timer;
    void setupBLE();

    // --- BLE State Machine ---
    enum BleRestartState { BLE_IDLE, BLE_DISCONNECTED, BLE_RESTARTING };
    BleRestartState ble_restart_state;
    uint32_t ble_restart_timer;
};

#endif