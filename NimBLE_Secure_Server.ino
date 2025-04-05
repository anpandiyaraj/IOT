#include <NimBLEDevice.h>
#include "esp_sleep.h"

// === Configuration ===
const uint64_t SLEEP_DURATION_US = 15ULL * 1000000ULL; // 15 seconds
const unsigned long AWAKE_DURATION_MS = 2000;          // 2 seconds

// === UUIDs ===
static const char* SERVICE_UUID         = "12345678-1234-1234-1234-123456789abc";
static const char* CHARACTERISTIC_UUID1 = "abcdefab-1234-1234-1234-abcdefabcdef"; // Client writes here
static const char* CHARACTERISTIC_UUID2 = "06e3746e-6ee8-4382-929a-163a34fee863"; // Server writes here

// === Globals ===
RTC_DATA_ATTR int bootCount = 0;
bool clientConnected = false;
unsigned long wakeTime;
NimBLECharacteristic* pNotifyCharacteristic;

// === Callback Classes ===
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer) {
    Serial.println("Client connected");
    clientConnected = true;

    // Optional greeting to client
    if (pNotifyCharacteristic) {
      pNotifyCharacteristic->setValue("Hello from server");
      pNotifyCharacteristic->notify();
    }
  }

  void onDisconnect(NimBLEServer* pServer) {
    Serial.println("Client disconnected");
    clientConnected = false;
  }

  void onAuthenticationComplete(ble_gap_conn_desc* desc) {
    Serial.println("Authentication complete");
  }
};

class SecureCharacteristicCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pCharacteristic) {
    std::string value = pCharacteristic->getValue();
    Serial.print("Received from client: ");
    Serial.println(value.c_str());

    // Append "ed" to message
    std::string modified = value + "ed";

    Serial.print("Modified and notifying client: ");
    Serial.println(modified.c_str());

    // Send modified string to client via notify
    if (pNotifyCharacteristic) {
      pNotifyCharacteristic->setValue(modified);
      pNotifyCharacteristic->notify();
    }
  }
};

// === Deep Sleep Handler ===
void goToSleep() {
  Serial.println("No client connected. Going to deep sleep...");
  delay(100); // Allow serial print to flush
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.printf("Boot #%d\n", ++bootCount);

  NimBLEDevice::init("SecureServer");
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  NimBLEServer* pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  NimBLEService* pService = pServer->createService(SERVICE_UUID);

  // Client sends data here
  NimBLECharacteristic* pWriteCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID1,
    NIMBLE_PROPERTY::WRITE
  );
  pWriteCharacteristic->setCallbacks(new SecureCharacteristicCallbacks());

  // Server responds here
  pNotifyCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID2,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );
  pNotifyCharacteristic->setValue("Waiting for client...");

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
    NimBLEDevice::deinit(true); // Clean up BLE stack

    esp_sleep_enable_timer_wakeup(SLEEP_DURATION_US);
    goToSleep();
  }
}
