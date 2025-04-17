#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLESecurity.h>
#include <map>
#include <string> // Include for std::string

// === UUID Definitions ===
#define SERVICE_UUID         "a1c658ed-1df2-4c5c-8477-708f714f01f7"
#define CHARACTERISTIC_UUID_1 "7dc6ca3d-f066-4bda-a742-4deb534b58d5" // Server response
#define CHARACTERISTIC_UUID_2 "f16c9c3c-fbcc-4a8c-b130-0e79948b8f82" // Client command

// Predefined passkey
#define PASSKEY "123456"

// === GPIO Pins for Commands ===
#define LOCK_PIN    16
#define UNLOCK_PIN  17
#define TRUNK_PIN   4
#define LOCATE_PIN  2
#define ELIGHT_PIN  15

// === Configuration ===
#define CONNECTION_RETRIES 3
#define DEBOUNCE_DELAY_MS 50
#define COMMAND_TIMEOUT_MS 2000
#define SCAN_DURATION_SEC 5
bool authenticated = false; // Declare before the class
// === Map GPIO Pins to Commands ===
enum class Command { LOCK, UNLOCK, TRUNK, LOCATE, ELIGHT, UNKNOWN };
const std::map<int, Command> pinToCommandMap = {
    {LOCK_PIN, Command::LOCK},
    {UNLOCK_PIN, Command::UNLOCK},
    {TRUNK_PIN, Command::TRUNK},
    {LOCATE_PIN, Command::LOCATE},
    {ELIGHT_PIN, Command::ELIGHT}
};

const std::map<Command, String> commandMap = {
    {Command::LOCK, "LOCK"},
    {Command::UNLOCK, "UNLOCK"},
    {Command::TRUNK, "TRUNK"},
    {Command::LOCATE, "LOCATE"},
    {Command::ELIGHT, "ELIGHT"}
};

// === Security Callbacks ===
class SecurityCallbacks : public BLESecurityCallbacks {
    uint32_t onPassKeyRequest() {
        Serial.printf("PassKey Requested. Returning: %s\n", PASSKEY);
        return atoi(PASSKEY);
    }

    void onPassKeyNotify(uint32_t pass_key) {
        Serial.printf("PassKey Notify: %u\n", pass_key);
    }

    bool onConfirmPIN(uint32_t passkey) override {
        Serial.printf("Client Confirm PIN: %u\n", passkey);
        return (passkey == atoi(PASSKEY));
    }

    bool onSecurityRequest() {
        Serial.println("Security Requested");
        return true;
    }

    void onAuthenticationComplete(esp_ble_auth_cmpl_t auth_cmpl) override {
        if (auth_cmpl.success) {
            Serial.println("Authentication Complete - Success");
            ::authenticated = true; // Use global scope resolution
        } else {
            Serial.printf("Authentication Complete - Failure, reason: 0x%02X\n", auth_cmpl.fail_reason);
            ::authenticated = false; // Use global scope resolution
        }
    }
};

// === BLE Globals ===
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteCharacteristic = nullptr;
BLERemoteCharacteristic* pReadCharacteristic = nullptr;
SecurityCallbacks* pSecurityCallbacks = nullptr;

// === Client Callbacks ===
class ClientCallbacks : public BLEClientCallbacks {
    void onConnect(BLEClient* pclient) {
        Serial.println("Client connected - initiating security...");
    }

    void onDisconnect(BLEClient* pclient) {
        Serial.println("Client disconnected.");
        authenticated = false;
    }
};

// === Functions ===

void printDeviceInfo(BLEAdvertisedDevice& device) {
    Serial.print("Found Device: ");
    Serial.print(device.getName().c_str());
    Serial.print(" [");
    Serial.print(device.getAddress().toString().c_str());
    Serial.print("] RSSI: ");
    Serial.println(device.getRSSI());
}

bool connectToServer() {
    Serial.println("Starting BLE scan...");
    BLEScan* pScan = BLEDevice::getScan();
    pScan->setActiveScan(true);
    pScan->setInterval(100);
    pScan->setWindow(99);

    for (int attempt = 0; attempt < CONNECTION_RETRIES; attempt++) {
        BLEScanResults* results = pScan->start(SCAN_DURATION_SEC, false);

        Serial.printf("Scan attempt %d found %d devices\n", attempt + 1, results->getCount());

        for (int i = 0; i < results->getCount(); i++) {
            BLEAdvertisedDevice advertisedDevice = results->getDevice(i);
            printDeviceInfo(advertisedDevice);

            if (advertisedDevice.haveServiceUUID() && advertisedDevice.getServiceUUID().equals(BLEUUID(SERVICE_UUID))) {
                Serial.println("Found target device. Connecting...");

                pClient = BLEDevice::createClient();
                pClient->setClientCallbacks(new ClientCallbacks());

                // Setup security before connecting
                BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT);
                pSecurityCallbacks = new SecurityCallbacks();
                BLEDevice::setSecurityCallbacks(pSecurityCallbacks);

                BLESecurity* pSecurity = new BLESecurity();
                pSecurity->setKeySize(16);
                pSecurity->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
                pSecurity->setCapability(ESP_IO_CAP_OUT);
                pSecurity->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

                if (pClient->connect(&advertisedDevice)) {
                    Serial.println("Connected to server!");

                    BLERemoteService* pService = pClient->getService(SERVICE_UUID);
                    if (pService) {
                        pReadCharacteristic = pService->getCharacteristic(CHARACTERISTIC_UUID_1);
                        pWriteCharacteristic = pService->getCharacteristic(CHARACTERISTIC_UUID_2);

                        if (pReadCharacteristic && pWriteCharacteristic) {
                            if (pReadCharacteristic->canRead() && pWriteCharacteristic->canWrite()) {
                                Serial.println("Service and characteristics found!");
                                return true;
                            } else {
                                Serial.println("One or more characteristics do not have required properties.");
                            }
                        }
                    }

                    Serial.println("Failed to find service or characteristics.");
                    pClient->disconnect();
                    delete pClient;
                    pClient = nullptr;
                } else {
                    Serial.println("Failed to connect to device.");
                }
            }
        }

        if (attempt < CONNECTION_RETRIES - 1) {
            Serial.printf("Retrying in 1 second... (%d/%d)\n", attempt + 1, CONNECTION_RETRIES);
            delay(1000);
        }
    }

    return false;
}

bool sendCommand(Command cmd) {
    if (!pClient || !pClient->isConnected() || !pWriteCharacteristic || !pReadCharacteristic) {
        Serial.println("Not connected or characteristics unavailable.");
        return false;
    }

    if (!authenticated) {
        Serial.println("Not authenticated!");
        return false;
    }

    String cmdStr = commandMap.at(cmd);
    Serial.println("Sending command: " + cmdStr);

    pWriteCharacteristic->writeValue(cmdStr.c_str(), cmdStr.length());

    unsigned long startTime = millis();
    while (millis() - startTime < COMMAND_TIMEOUT_MS) {
        if (pReadCharacteristic->canRead()) {
            std::string response = pReadCharacteristic->readValue().c_str(); // Convert String to std::string
            Serial.print("Server response: ");
            Serial.println(response.c_str());
            return true;
        }
        delay(100);
    }

    Serial.println("Timeout waiting for server response");
    return false;
}

void setupPins() {
    for (const auto& pinCommand : pinToCommandMap) {
        pinMode(pinCommand.first, INPUT_PULLDOWN);
        esp_sleep_enable_ext0_wakeup((gpio_num_t)pinCommand.first, HIGH);
    }
}

Command getCommandFromInput() {
    for (const auto& pinCommand : pinToCommandMap) {
        if (digitalRead(pinCommand.first) == HIGH) {
            delay(DEBOUNCE_DELAY_MS);
            if (digitalRead(pinCommand.first) == HIGH) {
                return pinCommand.second;
            }
        }
    }
    return Command::UNKNOWN;
}

void enterDeepSleep() {
    Serial.println("Entering deep sleep...");
    Serial.flush();
    esp_deep_sleep_start();
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\nStarting Secure BLE Client");

    setupPins();
    BLEDevice::init("BLE-Client");
}

void loop() {
    Command cmd = getCommandFromInput();
    if (cmd != Command::UNKNOWN) {
        Serial.println("Command detected: " + commandMap.at(cmd));

        if (connectToServer()) {
            if (sendCommand(cmd)) {
                Serial.println("Command executed successfully");
            } else {
                Serial.println("Failed to execute command");
            }

            pClient->disconnect();
            delete pClient;
            pClient = nullptr;
        } else {
            Serial.println("Failed to connect to server");
        }

        // Optional: Uncomment to enable deep sleep between commands
        // enterDeepSleep();
    }

    delay(100);
}
