#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEClient.h>
#include <map>

// === UUID Definitions ===
#define SERVICE_UUID          "a1c658ed-1df2-4c5c-8477-708f714f01f7"
#define CHARACTERISTIC_UUID_1 "7dc6ca3d-f066-4bda-a742-4deb534b58d5" // Server response
#define CHARACTERISTIC_UUID_2 "f16c9c3c-fbcc-4a8c-b130-0e79948b8f82" // Client command

// === GPIO Pins for Commands ===
#define LOCK_PIN    16
#define UNLOCK_PIN  17
#define TRUNK_PIN   4
#define LOCATE_PIN  2
#define ELIGHT_PIN  15

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

// === BLE Globals ===
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteCharacteristic = nullptr;
BLERemoteCharacteristic* pReadCharacteristic = nullptr;

// === Functions ===

/**
 * Scans for a BLE server advertising the specified service UUID.
 */
bool connectToServer() {
    BLEScan* pScan = BLEDevice::getScan();
    BLEScanResults* results = pScan->start(5, false); // Scan for 5 seconds

    for (int i = 0; i < results->getCount(); i++) {
        BLEAdvertisedDevice advertisedDevice = results->getDevice(i);

        if (advertisedDevice.isAdvertisingService(BLEUUID(SERVICE_UUID))) {
            Serial.println("Found target device. Connecting...");
            pClient = BLEDevice::createClient();

            if (pClient->connect(&advertisedDevice)) {
                Serial.println("Connected to server!");

                BLERemoteService* pService = pClient->getService(SERVICE_UUID);
                if (pService) {
                    pReadCharacteristic = pService->getCharacteristic(CHARACTERISTIC_UUID_1);
                    pWriteCharacteristic = pService->getCharacteristic(CHARACTERISTIC_UUID_2);

                    if (pReadCharacteristic && pWriteCharacteristic) {
                        Serial.println("Service and characteristics found!");
                        return true;
                    }
                }

                Serial.println("Failed to find service or characteristics.");
                pClient->disconnect();
            } else {
                Serial.println("Failed to connect.");
            }
        }
    }
    return false;
}

/**
 * Sends a command to the BLE server and retrieves its response.
 */
void sendCommand(Command cmd) {
    if (!pClient || !pClient->isConnected() || !pWriteCharacteristic || !pReadCharacteristic) {
        Serial.println("Not connected or characteristics unavailable.");
        return;
    }

    String cmdStr = commandMap.at(cmd);
    Serial.println("Sending command: " + cmdStr);
    pWriteCharacteristic->writeValue(cmdStr.c_str(), false);

    delay(200); // Allow time for server response

    if (pReadCharacteristic->canRead()) {
        String response = pReadCharacteristic->readValue();
        Serial.println("Server response: " + String(response.c_str()));
    }
}

/**
 * Configures GPIO pins for input.
 */
void setupPins() {
    for (const auto& pinCommand : pinToCommandMap) {
        pinMode(pinCommand.first, INPUT_PULLDOWN);
    }
}

/**
 * Determines which GPIO pin is pressed.
 */
Command getCommandFromInput() {
    for (const auto& pinCommand : pinToCommandMap) {
        if (digitalRead(pinCommand.first) == HIGH) {
            return pinCommand.second;
        }
    }
    return Command::UNKNOWN;
}

/**
 * Enters deep sleep mode to conserve power.
 */
void enterDeepSleep() {
    Serial.println("Entering deep sleep...");
    esp_deep_sleep_start();
}

// === Arduino Setup ===
void setup() {
    Serial.begin(115200);
    Serial.println("Starting BLE Client...");

    BLEDevice::init("");
    setupPins();
}

// === Arduino Loop ===
void loop() {
    Command cmd = getCommandFromInput();
    if (cmd != Command::UNKNOWN) {
        if (connectToServer()) {
            sendCommand(cmd);
            pClient->disconnect(); // Disconnect after sending the command
            pClient = nullptr;
        }
        enterDeepSleep();
    }

    delay(100); // Polling delay for GPIO inputs
}
