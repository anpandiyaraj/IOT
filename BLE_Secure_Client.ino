#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <BLEAddress.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLESecurity.h>
#include <esp_sleep.h>

// UUIDs
#define SERVICE_UUID         "726f72c1-055d-4f94-b090-c1afeec24782"
#define CHARACTERISTIC_UUID_1 "c1cf0c5d-d07f-4f7c-ad2e-9cb3e49286b"
#define CHARACTERISTIC_UUID_2 "b12523bb-5e18-41fa-a498-cceb16bb7628"

// Configurable variables
String serverMAC = "5C:01:3B:DD:9B:AA"; // Replace with your server's MAC
uint32_t blePasskey = 123456;
const int RSSI_THRESHOLD = -65; // Approx 30 feet
const unsigned long SCAN_INTERVAL = 60000; // 1 minute
const unsigned long INACTIVITY_TIMEOUT = 10000; // 10 seconds
const unsigned long BOOT_CONNECTION_TIMEOUT = 5000; // 5 seconds to attempt connection on boot

// === GPIO Pins for Commands (All RTC-capable) ===
#define LOCK_PIN    13  // RTC IO
#define UNLOCK_PIN  12  // RTC IO
#define TRUNK_PIN   4   // RTC IO 
#define LOCATE_PIN  2   // RTC IO
#define ELIGHT_PIN  15  // RTC IO
#define DEBOUNCE_DELAY_MS 50

// Command mapping
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

// BLE Objects
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pCharacteristic_1 = nullptr;
BLERemoteCharacteristic* pCharacteristic_2 = nullptr;

// State variables
bool connected = false;
bool shouldUnlock = false;
bool shouldLock = false;
bool bootUnlockAttempted = false;
unsigned long lastActivityTime = 0;
unsigned long lastScanTime = 0;
int lastRSSI = -100;
bool wasInRange = false;

// Configure all RTC-capable pins as wakeup sources
void configureWakeupSources() {
    uint64_t wakeupPinMask = 0;
    for (const auto& pair : pinToCommandMap) {
        wakeupPinMask |= (1ULL << pair.first);
    }
    esp_sleep_enable_ext1_wakeup(wakeupPinMask, ESP_EXT1_WAKEUP_ANY_HIGH);
    Serial.printf("Configured wakeup pins mask: 0x%llX\n", wakeupPinMask);
}

class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
        if (advertisedDevice.getAddress().toString() == serverMAC) {
            lastRSSI = advertisedDevice.getRSSI();
            Serial.print("Found server. RSSI: ");
            Serial.println(lastRSSI);
            
            if (lastRSSI > RSSI_THRESHOLD && !wasInRange) {
                shouldUnlock = true;
                wasInRange = true;
            } else if (lastRSSI <= RSSI_THRESHOLD && wasInRange) {
                shouldLock = true;
                wasInRange = false;
            }
            lastActivityTime = millis();
        }
    }
};

void notifyCallback(BLERemoteCharacteristic* pBLERemoteCharacteristic, 
                   uint8_t* pData, size_t length, bool isNotify) {
    Serial.print("Notification: ");
    Serial.write(pData, length);
    Serial.println();
    lastActivityTime = millis();
}

class ClientSecurityCallbacks : public BLESecurityCallbacks {
    void onPassKeyNotify(uint32_t pass_key) override {
        Serial.print("Passkey Notify: ");
        Serial.println(pass_key);
    }
    
    bool onSecurityRequest() override {
        Serial.println("Security Request");
        return true;
    }
    
    bool onConfirmPIN(uint32_t pin) override {
        Serial.print("Confirm PIN: ");
        Serial.println(pin);
        return (pin == blePasskey);
    }
    
    uint32_t onPassKeyRequest() override { 
        Serial.println("PassKey Request");
        return blePasskey; 
    }
    
    void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) override {
        Serial.println(cmpl.success ? "✅ Authenticated" : "❌ Auth failed");
        lastActivityTime = millis();
    }
};

bool connectToServer() {
    if (connected) return true;
    
    Serial.println("Connecting to server...");
    lastActivityTime = millis();
    
    if (pClient == nullptr) {
        pClient = BLEDevice::createClient();
        BLEDevice::setSecurityCallbacks(new ClientSecurityCallbacks());
        
        BLESecurity* pSecurity = new BLESecurity();
        pSecurity->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
        pSecurity->setCapability(ESP_IO_CAP_OUT);
    }

    BLEAddress serverAddress(serverMAC.c_str());
    if (!pClient->connect(serverAddress)) {
        Serial.println("❌ Connection failed");
        return false;
    }

    BLERemoteService* pRemoteService = pClient->getService(SERVICE_UUID);
    if (pRemoteService == nullptr) {
        pClient->disconnect();
        return false;
    }

    pCharacteristic_1 = pRemoteService->getCharacteristic(CHARACTERISTIC_UUID_1);
    pCharacteristic_2 = pRemoteService->getCharacteristic(CHARACTERISTIC_UUID_2);

    if (pCharacteristic_1 && pCharacteristic_1->canNotify()) {
        pCharacteristic_1->registerForNotify(notifyCallback);
    }

    connected = true;
    Serial.println("✅ Connected");
    lastActivityTime = millis();
    return true;
}

void sendCommand(const String& command) {
    if (!connectToServer()) {
        Serial.println("⚠️ Cannot send command: connection failed");
        return;
    }
    
    if (pCharacteristic_2 == nullptr) return;
    
    pCharacteristic_2->writeValue(command.c_str(), command.length());
    Serial.print("Sent: ");
    Serial.println(command);
    lastActivityTime = millis();
    
    // Disconnect after sending command to save power
    delay(100);
    pClient->disconnect();
    connected = false;
}

void scanForServer() {
    BLEScan* pScan = BLEDevice::getScan();
    pScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
    pScan->setActiveScan(true);
    pScan->start(5, false); // Scan for 5 seconds
    lastActivityTime = millis();
}

Command checkButtonPress() {
    for (const auto& pair : pinToCommandMap) {
        if (digitalRead(pair.first) == HIGH) {
            unsigned long currentTime = millis();
            if (currentTime - lastActivityTime > DEBOUNCE_DELAY_MS) {
                lastActivityTime = currentTime;
                return pair.second;
            }
        }
    }
    return Command::UNKNOWN;
}

void enterDeepSleep() {
    Serial.println("Entering deep sleep...");
    delay(100); // Allow serial to flush
    
    // Configure wakeup sources before sleeping
    configureWakeupSources();
    
    // Set all pins to low power state
    for (const auto& pair : pinToCommandMap) {
        digitalWrite(pair.first, LOW);
        pinMode(pair.first, INPUT_PULLDOWN);
    }
    
    esp_deep_sleep_start();
}

void attemptBootUnlock() {
    if (bootUnlockAttempted) return;
    
    Serial.println("Attempting boot unlock...");
    unsigned long startTime = millis();
    
    while (millis() - startTime < BOOT_CONNECTION_TIMEOUT) {
        if (connectToServer()) {
            sendCommand("UNLOCK");
            break;
        }
        delay(500);
    }
    
    bootUnlockAttempted = true;
    lastActivityTime = millis();
}

void setup() {
    Serial.begin(115200);
    Serial.println("🚀 Starting BLE Client");
    
    // Initialize all command pins
    for (const auto& pair : pinToCommandMap) {
        pinMode(pair.first, INPUT_PULLDOWN);
    }
    
    BLEDevice::init("BLE_Client");
    lastActivityTime = millis();
    
    // Attempt unlock on boot
    attemptBootUnlock();
}

void loop() {
    // Check for button presses
    Command pressedCommand = checkButtonPress();
    if (pressedCommand != Command::UNKNOWN) {
        Serial.print("Button pressed: ");
        Serial.println(commandMap.at(pressedCommand).c_str());
        sendCommand(commandMap.at(pressedCommand));
    }

    // Periodic scan and auto lock/unlock
    if (millis() - lastScanTime > SCAN_INTERVAL) {
        lastScanTime = millis();
        scanForServer();
        
        if (shouldUnlock) {
            sendCommand("UNLOCK");
            shouldUnlock = false;
        }
        
        if (shouldLock) {
            sendCommand("LOCK");
            shouldLock = false;
        }
    }

    // Enter deep sleep if inactive for too long
    if (millis() - lastActivityTime > INACTIVITY_TIMEOUT && !connected) {
        enterDeepSleep();
    }

    delay(50);
}
