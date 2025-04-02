#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "esp_gap_ble_api.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_sleep.h"

// Initialize all pointers
BLEServer* pServer = NULL;
BLECharacteristic* pCharacteristic_1 = NULL;
BLECharacteristic* pCharacteristic_2 = NULL;
BLEDescriptor *pDescr_1;
BLE2902 *pBLE2902_1;
BLE2902 *pBLE2902_2;

// Connection state
bool deviceConnected = false;
bool oldDeviceConnected = false;
bool isAuthenticated = false;
uint16_t currentConnId = 0;
unsigned long lastDisconnectTime = 0;

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
    {"ELIGHT","Emergency Light"}
};

// UUIDs
#define SERVICE_UUID          "a1c658ed-1df2-4c5c-8477-708f714f01f7"
#define CHARACTERISTIC_UUID_1 "7dc6ca3d-f066-4bda-a742-4deb534b58d5"
#define CHARACTERISTIC_UUID_2 "f16c9c3c-fbcc-4a8c-b130-0e79948b8f82"

// Predefined passkey
#define PASSKEY "123456"

#define LOCK_PIN    25  // GPIO pin for Lock
#define UNLOCK_PIN  26  // GPIO pin for Unlock
#define TRUNK_PIN   27  // GPIO pin for Trunk
#define LOCATE_PIN  14  // GPIO pin for Locate/Siren
#define ELIGHT_PIN  13  // GPIO pin for Emergency Light

// GAP callback to handle connection events
void gapCallback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
    switch(event) {
        case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
            char macStr[18];
            sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
                    param->update_conn_params.bda[0], param->update_conn_params.bda[1],
                    param->update_conn_params.bda[2], param->update_conn_params.bda[3],
                    param->update_conn_params.bda[4], param->update_conn_params.bda[5]);
            Serial.print("Device connected: ");
            Serial.println(macStr);
            break;

        case ESP_GAP_BLE_AUTH_CMPL_EVT:
            if(param->ble_security.auth_cmpl.success) {
                Serial.println("Authentication successful");
                isAuthenticated = true;
            } else {
                Serial.println("Authentication failed - disconnecting");
                isAuthenticated = false;
                if(pServer != nullptr && currentConnId > 0) {
                    pServer->disconnect(currentConnId);
                }
            }
            break;
    }
}

class MyServerCallbacks: public BLEServerCallbacks {
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
      lastDisconnectTime = millis();
      Serial.println("Device disconnected");
    }
};

class CharacteristicCallBack: public BLECharacteristicCallbacks {
private:
    void controlPin(uint8_t pin, float seconds) {
        digitalWrite(pin, HIGH);
        delay(seconds * 1000);  // Convert seconds to milliseconds
        digitalWrite(pin, LOW);
    }

    void blinkPin(uint8_t pin, float seconds) {
        unsigned long endTime = millis() + (seconds * 1000);
        while (millis() < endTime) {
            digitalWrite(pin, HIGH);
            delay(100);
            digitalWrite(pin, LOW);
            delay(100);
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

    void onWrite(BLECharacteristic *pChar) override {
        if (!isAuthenticated) {
            Serial.println("Unauthorized access attempt - ignoring write");
            return;
        }

        String receivedValue = pChar->getValue().c_str();
        Serial.println("Received command: " + receivedValue);

        Command cmd = parseCommand(receivedValue);
        String response;

        // Look up response in map
        auto it = COMMAND_RESPONSES.find(receivedValue);
        if (it != COMMAND_RESPONSES.end()) {
            response = it->second;
            Serial.println("Executing command: " + receivedValue);

            // Here you can add actual hardware control logic based on the command
             switch(cmd) {
                case Command::LOCK:
                    controlPin(LOCK_PIN, 0.5);  // Activate for 0.5 seconds
                    break;
                case Command::UNLOCK:
                    controlPin(UNLOCK_PIN, 0.5);  // Activate for 0.5 seconds
                    break;
                case Command::TRUNK:
                    controlPin(TRUNK_PIN, 1.0);  // Activate for 1 second
                    break;
                case Command::LOCATE:
                    blinkPin(LOCATE_PIN, 3.0);  // Blink for 3 seconds
                    break;
                case Command::ELIGHT:
                    // Execute ELIGHT command without sending response
                    controlPin(ELIGHT_PIN, 30);
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

    void onRead(BLECharacteristic *pChar) override {
        if (!isAuthenticated) {
            Serial.println("Unauthorized access attempt - blocking read");
            pChar->setValue("");
            return;
        }
    }
};

class MySecurityCallbacks : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override {
    Serial.println("Passkey requested");
    return atoi(PASSKEY);
  }

  void onPassKeyNotify(uint32_t pass_key) override {
    Serial.printf("Passkey Notify: %d\n", pass_key);
  }

  bool onConfirmPIN(uint32_t pass_key) override {
    Serial.printf("Confirming passkey: %d\n", pass_key);
    bool match = (pass_key == atoi(PASSKEY));
    if (!match) {
      Serial.println("Passkey mismatch - rejecting");
      isAuthenticated = false;
      if(pServer != nullptr && currentConnId > 0) {
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
      if(pServer != nullptr && currentConnId > 0) {
        pServer->disconnect(currentConnId);
      }
    }
  }

  bool onSecurityRequest() override {
    Serial.println("Security request received - requiring authentication");
    return true;
  }
};

void setup() {
  Serial.begin(115200);

  pinMode(LOCK_PIN, OUTPUT);
  pinMode(UNLOCK_PIN, OUTPUT);
  pinMode(TRUNK_PIN, OUTPUT);
  pinMode(LOCATE_PIN, OUTPUT);
  pinMode(ELIGHT_PIN, OUTPUT);

  // Initialize all pins to LOW
  digitalWrite(LOCK_PIN, LOW);
  digitalWrite(UNLOCK_PIN, LOW);
  digitalWrite(TRUNK_PIN, LOW);
  digitalWrite(LOCATE_PIN, LOW);
  digitalWrite(ELIGHT_PIN, LOW);
  // Debug print
  Serial.println("GPIO pins initialized");

  // Register the GAP callback
  esp_ble_gap_register_callback(gapCallback);

  // Create the BLE Device
  BLEDevice::init("ESP32_Lock");

  // Set the security parameters and callbacks
  BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT_MITM);
  BLEDevice::setSecurityCallbacks(new MySecurityCallbacks());

  // Set up BLE security
  BLESecurity *pSecurity = new BLESecurity();
  pSecurity->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
  pSecurity->setCapability(ESP_IO_CAP_OUT);
  pSecurity->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  pSecurity->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  pSecurity->setKeySize(16);

  // Set static passkey
  uint32_t passkey = atoi(PASSKEY);
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_STATIC_PASSKEY, &passkey, sizeof(uint32_t));

  // Enable security request on connection
  uint8_t auth_option = ESP_BLE_ONLY_ACCEPT_SPECIFIED_AUTH_ENABLE;
  esp_ble_gap_set_security_param(ESP_BLE_SM_ONLY_ACCEPT_SPECIFIED_SEC_AUTH, &auth_option, sizeof(uint8_t));

  // Create the BLE Server
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  // Create the BLE Service
  BLEService *pService = pServer->createService(SERVICE_UUID);

  // Create characteristics with security permissions
  pCharacteristic_1 = pService->createCharacteristic(
                      CHARACTERISTIC_UUID_1,
                      BLECharacteristic::PROPERTY_NOTIFY |
                      BLECharacteristic::PROPERTY_READ
                    );
  pCharacteristic_1->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED);

  pCharacteristic_2 = pService->createCharacteristic(
                      CHARACTERISTIC_UUID_2,
                      BLECharacteristic::PROPERTY_READ   |
                      BLECharacteristic::PROPERTY_WRITE  |
                      BLECharacteristic::PROPERTY_NOTIFY
                    );
  pCharacteristic_2->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED);

  // Create descriptors
  pDescr_1 = new BLEDescriptor((uint16_t)0x2901);
  pDescr_1->setValue("Ble Communication");
  pCharacteristic_1->addDescriptor(pDescr_1);

  pBLE2902_1 = new BLE2902();
  pBLE2902_1->setNotifications(true);
  pCharacteristic_1->addDescriptor(pBLE2902_1);

  pBLE2902_2 = new BLE2902();
  pBLE2902_2->setNotifications(true);
  pCharacteristic_2->addDescriptor(pBLE2902_2);

  // Set callbacks
  pCharacteristic_1->setCallbacks(new CharacteristicCallBack());
  pCharacteristic_2->setCallbacks(new CharacteristicCallBack());

  // Start service
  pService->start();

  // Configure advertising
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMaxPreferred(0x12);
  pAdvertising->setMinInterval(0x20); // Set minimum advertising interval
  pAdvertising->setMaxInterval(0x40); // Set maximum advertising interval

  // Set security parameters
  esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_MITM_BOND;
  esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));

  // Start advertising
  BLEDevice::startAdvertising();
  Serial.println("Waiting for a client connection with passkey authentication...");
}

void loop() {
    if (!deviceConnected && oldDeviceConnected) {
        delay(500);
        pServer->startAdvertising();
        Serial.println("Start advertising");
        oldDeviceConnected = deviceConnected;
    }
    if (deviceConnected && !oldDeviceConnected) {
        oldDeviceConnected = deviceConnected;
    }

    // Check if no clients are connected for 2 minutes
    if (!deviceConnected && millis() - lastDisconnectTime > 120000) {
       // Serial.println("No clients connected for 2 minutes, entering light sleep...");
        esp_sleep_enable_timer_wakeup(8000000); // Wake up after 8 second
        delay(2000000);
        esp_deep_sleep_start();
    }
}
