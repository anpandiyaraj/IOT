#include <NimBLEDevice.h>
#include "esp_sleep.h"
#include <map>

// === Configuration ===
const uint64_t SLEEP_DURATION_US = 15ULL * 1000000ULL; // 15 seconds
const unsigned long AWAKE_DURATION_MS = 2000;          // 2 seconds

// === UUIDs ===
static const char* SERVICE_UUID         = "12345678-1234-1234-1234-123456789abc";
static const char* CHARACTERISTIC_UUID1 = "abcdefab-1234-1234-1234-abcdefabcdef"; // Client writes here
static const char* CHARACTERISTIC_UUID2 = "06e3746e-6ee8-4382-929a-163a34fee863"; // Server writes here

// === Command Enum and Pin Map ===
enum class Command {
  LOCK,
  UNLOCK,
  TRUNK,
  LOCATE,
  ELIGHT,
  UNKNOWN
};

const std::map<String, Command> COMMAND_MAP = {
  {"LOCK", Command::LOCK},
  {"UNLOCK", Command::UNLOCK},
  {"TRUNK", Command::TRUNK},
  {"LOCATE", Command::LOCATE},
  {"ELIGHT", Command::ELIGHT},
};

const std::map<Command, String> COMMAND_RESPONSES = {
  {Command::LOCK,   "Door Locked"},
  {Command::UNLOCK, "Door Unlocked"},
  {Command::TRUNK,  "Trunk Released"},
  {Command::LOCATE, "Located"},
  {Command::ELIGHT, "Emergency Light"},
};

#define LOCK_PIN      25
#define UNLOCK_PIN    26
#define TRUNK_PIN     27
#define LOCATE_PIN    14
#define ELIGHT_PIN    13

// === Globals ===
RTC_DATA_ATTR int bootCount = 0;
bool clientConnected = false;
unsigned long wakeTime;
NimBLECharacteristic* pNotifyCharacteristic;

// === Utility Functions ===
void controlPin(uint8_t pin, float seconds) {
  digitalWrite(pin, HIGH);
  delay(seconds * 1000);
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

Command parseCommand(const String& cmdStr) {
  auto it = COMMAND_MAP.find(cmdStr);
  return it != COMMAND_MAP.end() ? it->second : Command::UNKNOWN;
}

// === Callback Classes ===
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer) {
    Serial.println("Client connected");
    clientConnected = true;
    if (pNotifyCharacteristic) {
      pNotifyCharacteristic->setValue("Hello from server");
      pNotifyCharacteristic->notify();
    }
  }

  void onDisconnect(NimBLEServer* pServer) {
    Serial.println("Client disconnected");
    clientConnected = false;
  }
};

class SecureCharacteristicCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pCharacteristic) {
    std::string value = pCharacteristic->getValue();
    String commandStr(value.c_str());
    Serial.print("Received command: ");
    Serial.println(commandStr);

    Command cmd = parseCommand(commandStr);
    String response;

    switch (cmd) {
      case Command::LOCK:
        controlPin(LOCK_PIN, 0.5);
        response = COMMAND_RESPONSES.at(cmd);
        break;
      case Command::UNLOCK:
        controlPin(UNLOCK_PIN, 0.5);
        response = COMMAND_RESPONSES.at(cmd);
        break;
      case Command::TRUNK:
        controlPin(TRUNK_PIN, 0.5);
        response = COMMAND_RESPONSES.at(cmd);
        break;
      case Command::LOCATE:
        blinkPin(LOCATE_PIN, 2);
        response = COMMAND_RESPONSES.at(cmd);
        break;
      case Command::ELIGHT:
        blinkPin(ELIGHT_PIN, 3);
        response = COMMAND_RESPONSES.at(cmd);
        break;
      default:
        response = "Invalid Command";
        break;
    }

    Serial.println("Sending response: " + response);
    if (pNotifyCharacteristic) {
      pNotifyCharacteristic->setValue(response.c_str());
      pNotifyCharacteristic->notify();
    }
  }
};

// === Deep Sleep Handler ===
void goToSleep() {
  Serial.println("No client connected. Going to deep sleep...");
  delay(100);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.printf("Boot #%d\n", ++bootCount);

  // Initialize GPIOs
  pinMode(LOCK_PIN, OUTPUT);
  pinMode(UNLOCK_PIN, OUTPUT);
  pinMode(TRUNK_PIN, OUTPUT);
  pinMode(LOCATE_PIN, OUTPUT);
  pinMode(ELIGHT_PIN, OUTPUT);

  digitalWrite(LOCK_PIN, LOW);
  digitalWrite(UNLOCK_PIN, LOW);
  digitalWrite(TRUNK_PIN, LOW);
  digitalWrite(LOCATE_PIN, LOW);
  digitalWrite(ELIGHT_PIN, LOW);

  // BLE Setup
  NimBLEDevice::init("SecureServer");
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  NimBLEServer* pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  NimBLEService* pService = pServer->createService(SERVICE_UUID);

  NimBLECharacteristic* pWriteCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID1, NIMBLE_PROPERTY::WRITE);
  pWriteCharacteristic->setCallbacks(new SecureCharacteristicCallbacks());

  pNotifyCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID2, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  pNotifyCharacteristic->setValue("Waiting for command...");

  pService->start();
  NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->start();

  Serial.println("Advertising started");
  wakeTime = millis();
}

void loop() {
  if (!clientConnected && millis() - wakeTime > AWAKE_DURATION_MS) {
    NimBLEDevice::getAdvertising()->stop();
    NimBLEDevice::deinit(true);
    esp_sleep_enable_timer_wakeup(SLEEP_DURATION_US);
    goToSleep();
  }
}
