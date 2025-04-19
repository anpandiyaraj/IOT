#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "esp_gap_ble_api.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_sleep.h"
#include <map>
#include <string>

// BLE Objects
BLEServer* pServer = NULL;
BLECharacteristic* pCharacteristic_1 = NULL;
BLECharacteristic* pCharacteristic_2 = NULL;
BLEDescriptor* pDescr_1;
BLE2902* pBLE2902_1;
BLE2902* pBLE2902_2;

// Connection state
bool deviceConnected = false;
bool oldDeviceConnected = false;
bool isAuthenticated = false;
uint16_t currentConnId = 0;
unsigned long lastActivityTime = 0;
bool initialBoot = true;
unsigned long bootTime = 0;

// New variable to track lock state
bool isLocked = false;

// Configuration
const unsigned long WAKEUP_INTERVAL = 10000000;    // 10 seconds
const unsigned long WAKEUP_DELAY = 1000;         // 1 second delay after wakeup
const unsigned long CONNECTION_TIMEOUT = 2000;     // 2 seconds to find connection
const unsigned long INITIAL_BOOT_DELAY = 120000; // 2 minutes initial awake time
const String PASSKEY = "123456";
const unsigned long DISCONNECT_LOCK_DELAY = 1000; // 1 second delay before auto-lock

// UUIDs
#define SERVICE_UUID            "726f72c1-055d-4f94-b090-c1afeec24782"
#define CHARACTERISTIC_UUID_1 "c1cf0c5d-d07f-4f7c-ad2e-9cb3e49286b4" // Server response
#define CHARACTERISTIC_UUID_2 "b12523bb-5e18-41fa-a498-cceb16bb7628" // Client command

// Pin Definitions
enum class Pin : uint8_t {
    LOCK = 12,
    UNLOCK = 13,
    TRUNK = 18,
    ELIGHT = 25,
    LOCATE = 19
};

// Command Definitions
enum class Command {
    LOCK,
    UNLOCK,
    TRUNK,
    LOCATE,
    ELIGHT,
    UNKNOWN
};

const std::map<String, String> COMMAND_RESPONSES = {
    {"LOCK", "Door Locked"},
    {"UNLOCK", "Door Unlocked"},
    {"LOCATE", "Located"},
    {"TRUNK", "Trunk Released"},
    {"ELIGHT", "Emergency Light"}
};

// Forward declaration
void performLock();

// ==================== GAP Callback ====================
void gapCallback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
    switch (event) {
        case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT: {
            char macStr[18];
            sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
                    param->update_conn_params.bda[0], param->update_conn_params.bda[1],
                    param->update_conn_params.bda[2], param->update_conn_params.bda[3],
                    param->update_conn_params.bda[4], param->update_conn_params.bda[5]);
            Serial.print("Device connected: ");
            Serial.println(macStr);
            break;
        }
        case ESP_GAP_BLE_AUTH_CMPL_EVT:
            if (param->ble_security.auth_cmpl.success) {
                Serial.println("Authentication successful");
                isAuthenticated = true;
            } else {
                Serial.println("Authentication failed - disconnecting");
                isAuthenticated = false;
                if (pServer != nullptr && currentConnId > 0) {
                    pServer->disconnect(currentConnId);
                }
            }
            break;
        default:
            break;
    }
}

// ==================== Server Callbacks ====================
class MyServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
        deviceConnected = true;
        currentConnId = pServer->getConnId();
        isAuthenticated = false;
        Serial.println("Client connected - waiting for authentication");
    };

    void onDisconnect(BLEServer* pServer) {
        deviceConnected = false;
        currentConnId = 0;
        isAuthenticated = false;
        lastActivityTime = millis();
        Serial.println("Device disconnected");
        BLEDevice::startAdvertising();

        // Add logic to automatically lock if not already locked
        if (!isLocked) {
            Serial.println("Device disconnected - performing automatic lock");
            delay(DISCONNECT_LOCK_DELAY); // Optional delay before locking
            performLock();
        }
    }
};

// ==================== Characteristic Callbacks ====================
class CharacteristicCallBack : public BLECharacteristicCallbacks {
private:
    void controlPin(Pin pin, float seconds) {
        digitalWrite(static_cast<uint8_t>(pin), HIGH);
        delay(static_cast<int>(seconds * 1000));
        digitalWrite(static_cast<uint8_t>(pin), LOW);
    }

    void blinkPin(Pin pin, float seconds) {
        unsigned long endTime = millis() + static_cast<unsigned long>(seconds * 1000);
        while (millis() < endTime) {
            digitalWrite(static_cast<uint8_t>(pin), HIGH);
            delay(300);
            digitalWrite(static_cast<uint8_t>(pin), LOW);
            delay(200);
        }
    }

public:
    Command parseCommand(const String& command) {
        String cmd = command;
        cmd.toUpperCase();

        if (cmd == "LOCK") return Command::LOCK;
        if (cmd == "UNLOCK") return Command::UNLOCK;
        if (cmd == "TRUNK") return Command::TRUNK;
        if (cmd == "LOCATE") return Command::LOCATE;
        if (cmd == "ELIGHT") return Command::ELIGHT;
        return Command::UNKNOWN;
    }

    void onWrite(BLECharacteristic* pChar) override {
        if (!isAuthenticated) {
            Serial.println("Unauthorized access attempt - ignoring write");
            return;
        }

        String receivedValue = pChar->getValue().c_str();
        Serial.println("Received command: " + receivedValue);

        Command cmd = parseCommand(receivedValue);
        String response;

        auto it = COMMAND_RESPONSES.find(receivedValue);
        if (it != COMMAND_RESPONSES.end()) {
            response = it->second;
            Serial.println("Executing command: " + receivedValue);

            switch (cmd) {
                case Command::LOCK:
                    controlPin(Pin::LOCK, 0.5);
                    isLocked = true; // Update lock state
                    break;
                case Command::UNLOCK:
                    controlPin(Pin::UNLOCK, 0.5);
                    isLocked = false; // Update lock state
                    break;
                case Command::TRUNK:
                    controlPin(Pin::TRUNK, 1.0);
                    break;
                case Command::LOCATE:
                    blinkPin(Pin::LOCATE, 3.0);
                    break;
                case Command::ELIGHT:
                    controlPin(Pin::ELIGHT, 30);
                    break;
                default:
                    break;
            }
        } else {
            response = "INVALID_COMMAND";
            Serial.println("Unknown command received");
        }

        pCharacteristic_1->setValue(response.c_str());
        pCharacteristic_1->notify();
        Serial.println("Response sent: " + response);
    }

    void onRead(BLECharacteristic* pChar) override {
        if (!isAuthenticated) {
            Serial.println("Unauthorized access attempt - blocking read");
            pChar->setValue("");
            return;
        }
    }
};

// ==================== Security Callbacks ====================
class MySecurityCallbacks : public BLESecurityCallbacks {
    uint32_t onPassKeyRequest() override {
        Serial.println("Passkey requested");
        return atoi(PASSKEY.c_str());
    }

    void onPassKeyNotify(uint32_t pass_key) override {
        Serial.printf("Passkey Notify: %d\n", pass_key);
    }

    bool onConfirmPIN(uint32_t pass_key) override {
        Serial.printf("Confirming passkey: %d\n", pass_key);
        bool match = (pass_key == atoi(PASSKEY.c_str()));
        if (!match) {
            Serial.println("Passkey mismatch - rejecting");
            isAuthenticated = false;
            if (pServer != nullptr && currentConnId > 0) {
                pServer->disconnect(currentConnId);
            }
        }
        return match;
    }

    void onAuthenticationComplete(esp_ble_auth_cmpl_t auth_cmpl) override {
        if (auth_cmpl.success) {
            Serial.println("Authentication successful");
            isAuthenticated = true;
        } else {
            Serial.println("Authentication failed - disconnecting");
            isAuthenticated = false;
            if (pServer != nullptr && currentConnId > 0) {
                pServer->disconnect(currentConnId);
            }
        }
    }

    bool onSecurityRequest() override {
        Serial.println("Security request received - requiring authentication");
        return true;
    }
};

// ==================== Helper function to perform the lock action
void performLock() {
    Serial.println("Performing lock action due to disconnection");
    digitalWrite(static_cast<uint8_t>(Pin::LOCK), HIGH);
    delay(500); // Simulate lock activation time
    digitalWrite(static_cast<uint8_t>(Pin::LOCK), LOW);
    isLocked = true;
}

// ==================== Setup ====================
void setup() {
    Serial.begin(115200);
    Serial.println("Initializing BLE Server");

    bootTime = millis();

    // Initialize GPIO pins
    pinMode(static_cast<uint8_t>(Pin::LOCK), OUTPUT);
    pinMode(static_cast<uint8_t>(Pin::UNLOCK), OUTPUT);
    pinMode(static_cast<uint8_t>(Pin::TRUNK), OUTPUT);
    pinMode(static_cast<uint8_t>(Pin::LOCATE), OUTPUT);
    pinMode(static_cast<uint8_t>(Pin::ELIGHT), OUTPUT);

    // Set initial pin states (assuming unlocked at boot)
    digitalWrite(static_cast<uint8_t>(Pin::LOCK), LOW);
    digitalWrite(static_cast<uint8_t>(Pin::UNLOCK), LOW);
    digitalWrite(static_cast<uint8_t>(Pin::TRUNK), LOW);
    digitalWrite(static_cast<uint8_t>(Pin::LOCATE), LOW);
    digitalWrite(static_cast<uint8_t>(Pin::ELIGHT), LOW);

    // Initialize lock state
    isLocked = false;

    // Initialize BLE
    BLEDevice::init("ESP32_Lock");
    esp_ble_gap_register_callback(gapCallback);

    // Configure BLE Security
    BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT_MITM);
    BLEDevice::setSecurityCallbacks(new MySecurityCallbacks());

    BLESecurity* pSecurity = new BLESecurity();
    pSecurity->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
    pSecurity->setCapability(ESP_IO_CAP_OUT);
    pSecurity->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
    pSecurity->setKeySize(16);

    // Set static passkey
    uint32_t passkey = atoi(PASSKEY.c_str());
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_STATIC_PASSKEY, &passkey, sizeof(uint32_t));

    // Create BLE Server
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());

    // Create BLE Service
    BLEService* pService = pServer->createService(SERVICE_UUID);

    // Create Characteristics
    pCharacteristic_1 = pService->createCharacteristic(
        CHARACTERISTIC_UUID_1,
        BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ
    );
    pCharacteristic_1->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED);

    pCharacteristic_2 = pService->createCharacteristic(
        CHARACTERISTIC_UUID_2,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY
    );
    pCharacteristic_2->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED);

    // Create Descriptors
    pDescr_1 = new BLEDescriptor((uint16_t)0x2901);
    pDescr_1->setValue("BLE Communication");
    pCharacteristic_1->addDescriptor(pDescr_1);

    pBLE2902_1 = new BLE2902();
    pBLE2902_1->setNotifications(true);
    pCharacteristic_1->addDescriptor(pBLE2902_1);

    pBLE2902_2 = new BLE2902();
    pBLE2902_2->setNotifications(true);
    pCharacteristic_2->addDescriptor(pBLE2902_2);

    // Set Callbacks
    pCharacteristic_1->setCallbacks(new CharacteristicCallBack());
    pCharacteristic_2->setCallbacks(new CharacteristicCallBack());

    // Start Service
    pService->start();

    // Start Advertising
    BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x06);  // Functions that help with iPhone connections issue
    pAdvertising->setMinPreferred(0x12);
    BLEDevice::startAdvertising();
    Serial.println("Waiting for a client connection...");

    lastActivityTime = millis();
    delay(WAKEUP_DELAY);
}

// ==================== Main Loop ====================
void loop() {
    unsigned long currentTime = millis();

    // Handle initial boot period
    if (initialBoot) {
        if (currentTime - bootTime < INITIAL_BOOT_DELAY) {
            // Still in initial boot period - stay awake regardless of connection
            if (!deviceConnected && (currentTime - lastActivityTime > 10000)) {
                // Restart advertising every 10 seconds during initial boot period
                BLEDevice::startAdvertising();
                Serial.println("Advertising during initial boot period");
                lastActivityTime = currentTime;
            }
            delay(10);
            return; // Skip normal sleep logic during initial boot
        } else {
            // Initial boot period ended
            initialBoot = false;
            lastActivityTime = currentTime; // Reset timer for normal operation
            Serial.println("Initial boot period ended - entering normal operation");
        }
    }

    // Track connection state changes
    if (!deviceConnected && oldDeviceConnected) {
        delay(500); // Give the bluetooth stack time to cleanup
        oldDeviceConnected = deviceConnected;
        BLEDevice::startAdvertising();
        Serial.println("Advertising restarted");
        lastActivityTime = currentTime; // Reset timer when starting advertising
    }

    if (deviceConnected && !oldDeviceConnected) {
        oldDeviceConnected = deviceConnected;
        lastActivityTime = currentTime; // Reset timer when device connects
    }

    // Normal operation sleep logic (after initial boot period)
    if (!initialBoot && !deviceConnected && (currentTime - lastActivityTime > CONNECTION_TIMEOUT)) {
        Serial.println("No connection established - entering deep sleep");
        esp_sleep_enable_timer_wakeup(WAKEUP_INTERVAL);
        esp_deep_sleep_start();
    }

    delay(10); // Prevent watchdog issues
}
