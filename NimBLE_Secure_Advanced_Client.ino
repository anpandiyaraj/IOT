#include <Arduino.h>
#include <NimBLEDevice.h>
#include <map>
#include "esp_sleep.h"

// ========================= CONFIGURABLE CONSTANTS =========================
const int LOCK_PIN         = 25;
const int UNLOCK_PIN       = 26;
const int ACTION_PIN       = 34;

const char* SERVICE_UUID          = "a1c658ed-1df2-4c5c-8477-708f714f01f7";
const char* CHARACTERISTIC_UUID_1 = "7dc6ca3d-f066-4bda-a742-4deb534b58d5";  // Response
const char* CHARACTERISTIC_UUID_2 = "f16c9c3c-fbcc-4a8c-b130-0e79948b8f82";  // Command

const uint32_t SCAN_DURATION_MS          = 5000;
const uint32_t BUTTON_DEBOUNCE_DELAY_MS  = 10;
const uint32_t COMMAND_REPEAT_DELAY_MS   = 500;
const uint32_t CLICK_DETECT_WINDOW_MS    = 400;
const uint32_t LONG_PRESS_THRESHOLD_MS   = 1000;

const uint32_t INACTIVITY_SLEEP_MS       = 2 * 60 * 1000;  // 2 minutes
const uint64_t PERIODIC_WAKEUP_US        = 3 * 60 * 1000000ULL; // 3 minutes in microseconds

const int WAKEUP_REASON_UNLOCK = 0x01;

// ========================= ENUM AND GLOBALS =========================
enum class Command {
  LOCK, UNLOCK, TRUNK, LOCATE, ELIGHT, UNKNOWN
};

std::map<Command, String> commandMap = {
  {Command::LOCK,   "LOCK"},
  {Command::UNLOCK, "UNLOCK"},
  {Command::TRUNK,  "TRUNK"},
  {Command::LOCATE, "LOCATE"},
  {Command::ELIGHT, "ELIGHT"}
};

NimBLERemoteCharacteristic* pCommandChar = nullptr;
NimBLERemoteCharacteristic* pResponseChar = nullptr;

unsigned long lastActionTime = 0;
bool alreadyUnlocked = false;

// ========================= BLE CALLBACKS =========================
class ClientCallbacks : public NimBLEClientCallbacks {
  void onPassKeyEntry(NimBLEConnInfo& connInfo) override {
    Serial.println("Server Passkey Entry");
    NimBLEDevice::injectPassKey(connInfo, 123456);
  }
} clientCallbacks;

// ========================= COMMAND HANDLING =========================
void sendCommand(Command cmd) {
  if (!pCommandChar) return;
  String cmdStr = commandMap.at(cmd);
  Serial.println("Sending command: " + cmdStr);
  pCommandChar->writeValue(cmdStr, false);
  lastActionTime = millis();

  if (pResponseChar && pResponseChar->canRead()) {
    String response = pResponseChar->readValue().c_str();
    Serial.println("Received: " + response);
    if (cmd == Command::UNLOCK && response == "UNLOCKED") {
      alreadyUnlocked = true;
    }
  }
}

// ========================= BUTTON DETECTION =========================
void setupPins() {
  pinMode(LOCK_PIN, INPUT_PULLDOWN);
  pinMode(UNLOCK_PIN, INPUT_PULLDOWN);
  pinMode(ACTION_PIN, INPUT);
}

void detectClicks() {
  static unsigned long pressStart = 0;
  static int clickCount = 0;
  static unsigned long lastClickTime = 0;
  static bool longPressActive = false;

  bool state = digitalRead(ACTION_PIN);

  if (state && !longPressActive) {
    if (pressStart == 0) pressStart = millis();
    if (millis() - pressStart > LONG_PRESS_THRESHOLD_MS) {
      sendCommand(Command::TRUNK);
      longPressActive = true;
    }
  } else if (!state) {
    if (pressStart > 0 && !longPressActive) {
      clickCount++;
      lastClickTime = millis();
    }
    pressStart = 0;
    longPressActive = false;
  }

  if (clickCount > 0 && millis() - lastClickTime > CLICK_DETECT_WINDOW_MS) {
    if (clickCount == 1) {
      sendCommand(Command::LOCATE);
    } else if (clickCount == 2) {
      sendCommand(Command::ELIGHT);
    }
    clickCount = 0;
  }
}

void checkGPIOCommands() {
  if (digitalRead(LOCK_PIN) == HIGH) {
    sendCommand(Command::LOCK);
    delay(COMMAND_REPEAT_DELAY_MS);
  }
  if (digitalRead(UNLOCK_PIN) == HIGH) {
    sendCommand(Command::UNLOCK);
    delay(COMMAND_REPEAT_DELAY_MS);
  }
  detectClicks();
}

// ========================= BLE SETUP =========================
void setupBLE() {
  NimBLEDevice::init("");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);

  NimBLEScan* pScan = NimBLEDevice::getScan();
  NimBLEScanResults results = pScan->getResults(SCAN_DURATION_MS);

  NimBLEUUID serviceUuid(SERVICE_UUID);

  for (int i = 0; i < results.getCount(); i++) {
    auto* device = const_cast<NimBLEAdvertisedDevice*>(results.getDevice(i));

    if (device->isAdvertisingService(serviceUuid)) {
      NimBLEClient* pClient = NimBLEDevice::createClient();
      pClient->setClientCallbacks(&clientCallbacks, false);

      if (pClient->connect(device)) {
        pClient->secureConnection();
        NimBLERemoteService* pService = pClient->getService(serviceUuid);
        if (pService) {
          pResponseChar = pService->getCharacteristic(CHARACTERISTIC_UUID_1);
          pCommandChar  = pService->getCharacteristic(CHARACTERISTIC_UUID_2);
          if (pCommandChar && pCommandChar->canWrite()) {
            Serial.println("BLE Connected & Ready");
            return;
          }
        }
      }
      NimBLEDevice::deleteClient(pClient);
    }
  }

  Serial.println("BLE server not found.");
}

// ========================= DEEP SLEEP & WAKEUP =========================
void checkInactivity() {
  if (millis() - lastActionTime > INACTIVITY_SLEEP_MS) {
    Serial.println("Entering deep sleep due to inactivity");
    esp_sleep_enable_ext0_wakeup((gpio_num_t)LOCK_PIN, 1);
    esp_sleep_enable_ext1_wakeup((1ULL << UNLOCK_PIN) | (1ULL << ACTION_PIN), ESP_EXT1_WAKEUP_ANY_HIGH);
    esp_sleep_enable_timer_wakeup(PERIODIC_WAKEUP_US);
    esp_deep_sleep_start();
  }
}

void handleWakeupReason() {
  esp_sleep_wakeup_cause_t wakeupReason = esp_sleep_get_wakeup_cause();
  switch (wakeupReason) {
    case ESP_SLEEP_WAKEUP_EXT0:
    case ESP_SLEEP_WAKEUP_EXT1:
      Serial.println("Woke up from button press");
      break;
    case ESP_SLEEP_WAKEUP_TIMER:
      Serial.println("Periodic wakeup");
      if (!alreadyUnlocked) {
        setupBLE();
        sendCommand(Command::UNLOCK);
      } else {
        Serial.println("Already unlocked, skipping UNLOCK");
      }
      esp_deep_sleep_start();  // Go back to sleep immediately
      break;
    default:
      Serial.println("Unknown wakeup reason");
      break;
  }
}

// ========================= MAIN =========================
void setup() {
  Serial.begin(115200);
  handleWakeupReason();
  setupPins();
  setupBLE();
  lastActionTime = millis();
}

void loop() {
  checkGPIOCommands();
  checkInactivity();
  delay(BUTTON_DEBOUNCE_DELAY_MS);
}
