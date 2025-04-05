#include <Arduino.h>
#include <NimBLEDevice.h>
#include <map> // <-- REQUIRED for std::map

// === UUID Definitions ===
#define SERVICE_UUID          "a1c658ed-1df2-4c5c-8477-708f714f01f7"
#define CHARACTERISTIC_UUID_1 "7dc6ca3d-f066-4bda-a742-4deb534b58d5" // Server response
#define CHARACTERISTIC_UUID_2 "f16c9c3c-fbcc-4a8c-b130-0e79948b8f82" // Client command

// === GPIO Pins for Commands ===
#define LOCK_PIN   32
#define UNLOCK_PIN 33
#define TRUNK_PIN  34
#define LOCATE_PIN 35
#define ELIGHT_PIN 36

enum class Command {
  LOCK, UNLOCK, TRUNK, LOCATE, ELIGHT, UNKNOWN
};

// Declare early to use in functions
const std::map<Command, String> commandMap = {
  {Command::LOCK, "LOCK"},
  {Command::UNLOCK, "UNLOCK"},
  {Command::TRUNK, "TRUNK"},
  {Command::LOCATE, "LOCATE"},
  {Command::ELIGHT, "ELIGHT"},
};

// === BLE Globals ===
NimBLERemoteCharacteristic* pWriteCharacteristic = nullptr;
NimBLERemoteCharacteristic* pReadCharacteristic = nullptr;

// === Callback for passkey authentication ===
class ClientCallbacks : public NimBLEClientCallbacks {
  void onPassKeyEntry(NimBLEConnInfo& connInfo) override {
    Serial.println("Passkey requested");
    NimBLEDevice::injectPassKey(connInfo, 123456);
  }
} clientCallbacks;

// === Setup GPIO pins as input with pulldown ===
void setupPins() {
  pinMode(LOCK_PIN, INPUT_PULLDOWN);
  pinMode(UNLOCK_PIN, INPUT_PULLDOWN);
  pinMode(TRUNK_PIN, INPUT_PULLDOWN);
  pinMode(LOCATE_PIN, INPUT_PULLDOWN);
  pinMode(ELIGHT_PIN, INPUT_PULLDOWN);
}

// === Connect and Setup BLE ===
NimBLEClient* connectToServer(NimBLEAdvertisedDevice* device) {
  NimBLEClient* pClient = NimBLEDevice::createClient();
  pClient->setClientCallbacks(&clientCallbacks, false);

  if (!pClient->connect(device)) {
    Serial.println("Failed to connect");
    NimBLEDevice::deleteClient(pClient);
    return nullptr;
  }

  pClient->secureConnection();

  NimBLERemoteService* pService = pClient->getService(SERVICE_UUID);
  if (!pService) {
    Serial.println("Service not found");
    pClient->disconnect();
    return nullptr;
  }

  pReadCharacteristic  = pService->getCharacteristic(CHARACTERISTIC_UUID_1);
  pWriteCharacteristic = pService->getCharacteristic(CHARACTERISTIC_UUID_2);

  if (!pReadCharacteristic || !pWriteCharacteristic) {
    Serial.println("Characteristics not found");
    pClient->disconnect();
    return nullptr;
  }

  Serial.println("Connected to BLE Server and characteristics found");
  return pClient;
}

// === Send Command and Wait for Response ===
void sendCommand(Command cmd) {
  if (!pWriteCharacteristic || !pReadCharacteristic) return;

  String cmdStr = commandMap.at(cmd);
  Serial.println("Sending: " + cmdStr);
  pWriteCharacteristic->writeValue(cmdStr.c_str(), false);

  delay(100); // Give server time to respond

  if (pReadCharacteristic->canRead()) {
    std::string response = pReadCharacteristic->readValue();
    Serial.println("Response: " + String(response.c_str()));
  }
}

// === Arduino Setup ===
void setup() {
  Serial.begin(115200);
  Serial.println("Starting Secure BLE Client");

  NimBLEDevice::init("");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9); // Max power
  NimBLEDevice::setSecurityAuth(true, true, true); // bonding, MITM, secure
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);

  setupPins();

  NimBLEScan* pScan = NimBLEDevice::getScan();
  NimBLEScanResults results = pScan->getResults(5000); // 5 seconds

  for (int i = 0; i < results.getCount(); i++) {
    NimBLEAdvertisedDevice* device = const_cast<NimBLEAdvertisedDevice*>(results.getDevice(i)); // <-- FIXED cast
    if (device->isAdvertisingService(NimBLEUUID(SERVICE_UUID))) {
      NimBLEClient* client = connectToServer(device);
      if (client) break;
    }
  }
}

// === Main Loop ===
void loop() {
  if (digitalRead(LOCK_PIN) == HIGH) {
    sendCommand(Command::LOCK);
    delay(1000);
  } else if (digitalRead(UNLOCK_PIN) == HIGH) {
    sendCommand(Command::UNLOCK);
    delay(1000);
  } else if (digitalRead(TRUNK_PIN) == HIGH) {
    sendCommand(Command::TRUNK);
    delay(1000);
  } else if (digitalRead(LOCATE_PIN) == HIGH) {
    sendCommand(Command::LOCATE);
    delay(1000);
  } else if (digitalRead(ELIGHT_PIN) == HIGH) {
    sendCommand(Command::ELIGHT);
    delay(1000);
  }

  delay(10); // Debounce delay
}
