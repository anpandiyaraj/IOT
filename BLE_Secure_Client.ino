#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <BLEAddress.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLESecurity.h>
#include <map>
#include <string>

// Configurable variables
String serverMAC = "5C:01:3B:9B:90:DD"; // Replace with your server's MAC
uint32_t blePasskey = 123456;

// UUIDs for the service and characteristics
#define SERVICE_UUID         "726f72c1-055d-4f94-b090-c1afeec24781"
#define CHARACTERISTIC_UUID_1 "c1cf0c5d-d07f-4f7c-ad2e-9cb3e49286b2" // Server response
#define CHARACTERISTIC_UUID_2 "b12523bb-5e18-41fa-a498-cceb16bb7623" // Client command

// === GPIO Pins for Commands ===
#define LOCK_PIN    16
#define UNLOCK_PIN  17
#define TRUNK_PIN   4
#define LOCATE_PIN  2
#define ELIGHT_PIN  15

// === Configuration ===
#define DEBOUNCE_DELAY_MS 50

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

BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pCharacteristic_1 = nullptr;
BLERemoteCharacteristic* pCharacteristic_2 = nullptr;
bool connected = false;

// Notification callback
void notifyCallback(BLERemoteCharacteristic* pBLERemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
    Serial.print("Notification received: ");
    for (int i = 0; i < length; i++) {
        Serial.print((char)pData[i]);
    }
    Serial.println();
}

// Security callbacks
class ClientSecurityCallbacks : public BLESecurityCallbacks {
    bool onSecurityRequest() {
        Serial.println("Security request received.");
        return true;
    }

    bool onConfirmPIN(uint32_t pass_key) {
        Serial.print("Confirm passkey: ");
        Serial.println(pass_key);
        return true;
    }

    uint32_t onPassKeyRequest() {
        Serial.println("Passkey requested.");
        return blePasskey;
    }

    void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) {
        if (cmpl.success) {
            Serial.println("✅ Authentication success.");
        } else {
            Serial.println("❌ Authentication failed.");
        }
    }

    void onPassKeyNotify(uint32_t pass_key) {
        Serial.print("Passkey notify: ");
        Serial.println(pass_key);
    }
};

bool connectToServer() {
    Serial.print("Connecting to server at ");
    Serial.println(serverMAC);

    BLEAddress bleServerAddress(serverMAC.c_str());

    pClient = BLEDevice::createClient();
    Serial.println(" - Client created.");

    BLESecurity* pSecurity = new BLESecurity();
    pSecurity->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
    pSecurity->setCapability(ESP_IO_CAP_OUT);  // Display-only (for passkey display)
    BLEDevice::setSecurityCallbacks(new ClientSecurityCallbacks());

    if (!pClient->connect(bleServerAddress)) {
        Serial.println("❌ Failed to connect to server.");
        connected = false;
        return false;
    }

    Serial.println("✅ Connected to server.");

    BLERemoteService* pRemoteService = pClient->getService(SERVICE_UUID);
    if (pRemoteService == nullptr) {
        Serial.println("❌ Failed to find service UUID.");
        pClient->disconnect();
        connected = false;
        return false;
    }

    pCharacteristic_1 = pRemoteService->getCharacteristic(CHARACTERISTIC_UUID_1);
    pCharacteristic_2 = pRemoteService->getCharacteristic(CHARACTERISTIC_UUID_2);

    if (pCharacteristic_1 && pCharacteristic_1->canNotify()) {
        pCharacteristic_1->registerForNotify(notifyCallback);
        Serial.println("🔔 Notification callback registered.");
    }

    if (pCharacteristic_2 && pCharacteristic_2->canWrite()) {
        Serial.println("✉️ Ready to send commands.");
    }

    connected = true;
    return true;
}

void sendCommand(const String& command) {
    if (connected && pCharacteristic_2 != nullptr) {
        pCharacteristic_2->writeValue((uint8_t*)command.c_str(), command.length(), false);
        Serial.print("📤 Sent command: ");
        Serial.println(command);
    } else {
        Serial.println("⚠️ Cannot send command: not connected.");
        if (pClient) {
            pClient->disconnect();
            connected = false;
            Serial.println("🔌 Disconnected.");
        }
    }
}

void setupPins() {
    for (const auto& pinCommand : pinToCommandMap) {
        pinMode(pinCommand.first, INPUT_PULLDOWN);
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

void setup() {
    Serial.begin(115200);
    Serial.println("🚀 Starting BLE Client with GPIO Control...");
    BLEDevice::init("ESP32_BLE_Client");
    setupPins();
    if (!connected) {
        connectToServer();
    }
}

void loop() {
    if (!connected) {
        Serial.println("Attempting to reconnect...");
        connectToServer();
        delay(5000); // Retry every 5 seconds
        return;
    }

    Command cmd = getCommandFromInput();
    if (cmd != Command::UNKNOWN) {
        Serial.println("Command detected: " + commandMap.at(cmd));
        sendCommand(commandMap.at(cmd));
    }
    delay(100); // Small delay
}
