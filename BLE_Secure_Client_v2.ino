#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEAddress.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLESecurity.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>
#include <map>
#include <string>
#include <vector>
#include <algorithm>

// ===== Configuration =====
const String serverMAC = "5C:01:3B:9D:90:BB"; // Replace with your server's MAC
const uint32_t blePasskey = 123456; // Matching your Java app's PASSKEY

// UUIDs for the service and characteristics
#define SERVICE_UUID             "726f72c1-055d-4f94-b090-c1afeec24782"
#define CHARACTERISTIC_UUID_RX   "c1cf0c5d-d07f-4f7c-ad2e-9cb3e49286b4" // Notification characteristic
#define CHARACTERISTIC_UUID_TX   "b12523bb-5e18-41fa-a498-cceb16bb7628" // Write characteristic

// GPIO Pins for Commands
#define LOCK_PIN               16
#define UNLOCK_PIN             17
#define TRUNK_PIN              4
#define LOCATE_PIN             2
#define RESET_PIN              13

// Timing and thresholds (matching Java app)
#define RSSI_THRESHOLD         -93      // Same as Java app's LOCK_THRESHOLD
#define RSSI_HISTORY_SIZE      5        // Same as Java app's RSSI_HISTORY_SIZE
#define RSSI_UPDATE_INTERVAL   1000     // Same as Java app's RSSI_UPDATE_INTERVAL
#define RSSI_CONFIRMATION_DELAY 2000    // Same as Java app's RSSI_CONFIRMATION_DELAY
#define DEBOUNCE_DELAY_MS      50
#define CONNECTION_TIMEOUT_MS  10000
#define CONNECTION_RETRY_DELAY_MS 5000
#define MAX_RAPID_RETRIES      3
#define LONG_SLEEP_DURATION_S  60
#define WATCHDOG_TIMEOUT_MS    30000

// Command enumeration
enum class Command { LOCK, UNLOCK, TRUNK, LOCATE, RESET, UNKNOWN };

// Lock status enumeration
enum class LockStatus { NONE, LOCKED, UNLOCKED };

// Pin to command mapping
const std::map<int, Command> pinToCommandMap = {
    {LOCK_PIN,   Command::LOCK},
    {UNLOCK_PIN, Command::UNLOCK},
    {TRUNK_PIN,  Command::TRUNK},
    {LOCATE_PIN, Command::LOCATE},
    {RESET_PIN,  Command::RESET}
};

// Command to string mapping
const std::map<Command, String> commandMap = {
    {Command::LOCK,   "LOCK"},
    {Command::UNLOCK, "UNLOCK"},
    {Command::TRUNK,  "TRUNK"},
    {Command::LOCATE, "LOCATE"},
    {Command::RESET,  "RESET"}
};

// Global variables
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pNotifyCharacteristic = nullptr;
BLERemoteCharacteristic* pWriteCharacteristic = nullptr;
bool connected = false;
std::vector<int> rssiHistory;
LockStatus lockStatus = LockStatus::NONE;
int connectionRetries = 0;
unsigned long lastRssiCheckTime = 0;
unsigned long lastRssiTriggerTime = 0;
bool isManualLock = false;

// ===== Watchdog Functions =====
void initWatchdog() {
    esp_task_wdt_config_t twdt_config = {
        .timeout_ms = WATCHDOG_TIMEOUT_MS,
        .idle_core_mask = (1 << 0)
    };
    
    if (esp_task_wdt_init(&twdt_config) != ESP_OK) {
        Serial.println("Failed to initialize watchdog");
    }
    
    if (esp_task_wdt_add(NULL) != ESP_OK) {
        Serial.println("Failed to add task to watchdog");
    }
    
    Serial.printf("Watchdog initialized with %d ms timeout\n", WATCHDOG_TIMEOUT_MS);
}

void feedWatchdog() {
    if (esp_task_wdt_reset() != ESP_OK) {
        Serial.println("Failed to feed watchdog");
    }
}

// ===== RSSI Functions =====
void addRSSI(int rssi) {
    if (rssi >= -100 && rssi <= 0) {
        rssiHistory.push_back(rssi);
        if (rssiHistory.size() > RSSI_HISTORY_SIZE) {
            rssiHistory.erase(rssiHistory.begin());
        }
    }
}

int getMedianRSSI() {
    if (rssiHistory.empty()) return -127;
    
    std::vector<int> sortedHistory = rssiHistory;
    std::sort(sortedHistory.begin(), sortedHistory.end());
    
    return sortedHistory[sortedHistory.size() / 2];
}

// ===== Auto Lock/Unlock Functions =====
bool shouldAutoUnlock(int rssi) {
    if (isManualLock) return false;
    return (rssi > RSSI_THRESHOLD);
}

bool shouldAutoLock(int rssi) {
    return (rssi < RSSI_THRESHOLD && lockStatus == LockStatus::UNLOCKED);
}

void handleDistanceChange(int rssi) {
    unsigned long currentTime = millis();
    if (currentTime - lastRssiTriggerTime < RSSI_CONFIRMATION_DELAY) {
        return;
    }

    if (shouldAutoUnlock(rssi) && 
        (lockStatus == LockStatus::NONE || lockStatus == LockStatus::LOCKED)) {
        Serial.println("Auto UNLOCK triggered by proximity");
        sendCommand(commandMap.at(Command::UNLOCK));
        lockStatus = LockStatus::UNLOCKED;
        lastRssiTriggerTime = currentTime;
    }
    else if (shouldAutoLock(rssi)) {
        Serial.println("Auto LOCK triggered by distance");
        sendCommand(commandMap.at(Command::LOCK));
        lockStatus = LockStatus::LOCKED;
        lastRssiTriggerTime = currentTime;
    }
}

void checkAutoLockUnlock() {
    if (!connected || !pClient->isConnected()) {
        return;
    }

    int currentRSSI = getMedianRSSI();
    Serial.print("RSSI: ");
    Serial.print(currentRSSI);
    Serial.print(", Status: ");
    switch(lockStatus) {
        case LockStatus::NONE: Serial.println("NONE"); break;
        case LockStatus::LOCKED: Serial.println("LOCKED"); break;
        case LockStatus::UNLOCKED: Serial.println("UNLOCKED"); break;
    }

    handleDistanceChange(currentRSSI);
}

// ===== BLE Functions =====
void notifyCallback(BLERemoteCharacteristic* pBLERemoteCharacteristic, 
                   uint8_t* pData, size_t length, bool isNotify) {
    Serial.print("Notification: ");
    for (int i = 0; i < length; i++) {
        Serial.print((char)pData[i]);
    }
    Serial.println();
    
    // Update lock status based on server response
    String response((char*)pData);
    if (response.indexOf("Unlocked") != -1) {
        lockStatus = LockStatus::UNLOCKED;
        isManualLock = false;
    } else if (response.indexOf("Locked") != -1) {
        lockStatus = LockStatus::LOCKED;
    }
}

class ClientSecurityCallbacks : public BLESecurityCallbacks {
    bool onSecurityRequest() {
        Serial.println("Security request received");
        return true;
    }

    bool onConfirmPIN(uint32_t pass_key) {
        Serial.print("Confirm passkey: ");
        Serial.println(pass_key);
        return (pass_key == blePasskey);
    }

    uint32_t onPassKeyRequest() {
        Serial.println("Passkey requested");
        return blePasskey;
    }

    void onPassKeyNotify(uint32_t pass_key) {
        Serial.print("Passkey Notify: ");
        Serial.println(pass_key);
        // You might want to handle this notification in a specific way
        // or simply acknowledge it.
    }

    void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) {
        if (cmpl.success) {
            Serial.println("✅ Authentication success");
            feedWatchdog();

            // Check if we should auto-unlock after authentication
            if (connected && shouldAutoUnlock(getMedianRSSI()) &&
                (lockStatus == LockStatus::NONE || lockStatus == LockStatus::LOCKED)) {
                Serial.println("Auto UNLOCK after authentication");
                sendCommand(commandMap.at(Command::UNLOCK));
                lockStatus = LockStatus::UNLOCKED;
            }
        } else {
            Serial.println("❌ Authentication failed");
        }
    }
};
class MyClientCallback : public BLEClientCallbacks {
    void onConnect(BLEClient* pclient) {
        connected = true;
        connectionRetries = 0;
        Serial.println("✅ Connected to server");
        feedWatchdog();
    }

    void onDisconnect(BLEClient* pclient) {
        connected = false;
        lockStatus = LockStatus::NONE;
        Serial.println("🔌 Disconnected from server");

        // Clean up BLE resources
        if (pNotifyCharacteristic) pNotifyCharacteristic = nullptr;
        if (pWriteCharacteristic) pWriteCharacteristic = nullptr;
        if (pClient) {
            pClient->disconnect();
            delete pClient;
            pClient = nullptr;
        }

        // Prepare for reconnection
        if (connectionRetries < MAX_RAPID_RETRIES) {
            Serial.printf("Retrying in %dms (attempt %d/%d)\n", 
                         CONNECTION_RETRY_DELAY_MS, 
                         connectionRetries + 1, 
                         MAX_RAPID_RETRIES);
            esp_sleep_enable_timer_wakeup(CONNECTION_RETRY_DELAY_MS * 1000);
        } else {
            Serial.printf("Max retries, sleeping for %d seconds\n", LONG_SLEEP_DURATION_S);
            esp_sleep_enable_timer_wakeup(LONG_SLEEP_DURATION_S * 1000000);
        }
        esp_deep_sleep_start();
    }
};

bool connectToServer() {
    Serial.print("Connecting to ");
    Serial.println(serverMAC);
    feedWatchdog();

    BLEAddress bleServerAddress(serverMAC.c_str());

    // Clean up previous connection if exists
    if (pClient) {
        pClient->disconnect();
        delete pClient;
        pClient = nullptr;
    }

    pClient = BLEDevice::createClient();
    pClient->setClientCallbacks(new MyClientCallback());

    // Configure security
    BLESecurity* pSecurity = new BLESecurity();
    pSecurity->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
    pSecurity->setCapability(ESP_IO_CAP_OUT);
    BLEDevice::setSecurityCallbacks(new ClientSecurityCallbacks());

    // Attempt connection with timeout
    unsigned long connectStart = millis();
    bool connectionSuccess = pClient->connect(bleServerAddress);
    
    while (!connectionSuccess && (millis() - connectStart < CONNECTION_TIMEOUT_MS)) {
        delay(100);
        feedWatchdog();
        connectionSuccess = pClient->isConnected();
    }

    if (!connectionSuccess) {
        Serial.println("❌ Failed to connect");
        connected = false;
        connectionRetries++;
        return false;
    }

    // Connection successful
    connectionRetries = 0;
    connected = true;

    // Get service
    BLERemoteService* pRemoteService = pClient->getService(SERVICE_UUID);
    if (!pRemoteService) {
        Serial.println("❌ Failed to find service UUID");
        pClient->disconnect();
        connected = false;
        return false;
    }

    // Get characteristics
    pNotifyCharacteristic = pRemoteService->getCharacteristic(CHARACTERISTIC_UUID_RX);
    pWriteCharacteristic = pRemoteService->getCharacteristic(CHARACTERISTIC_UUID_TX);

    if (!pNotifyCharacteristic || !pWriteCharacteristic) {
        Serial.println("❌ Failed to find characteristics");
        pClient->disconnect();
        connected = false;
        return false;
    }

    // Register for notifications
    if (pNotifyCharacteristic->canNotify()) {
        pNotifyCharacteristic->registerForNotify(notifyCallback);
        Serial.println("🔔 Notification callback registered");
    }

    // Get initial RSSI
    if (pClient->isConnected()) {
        delay(100); // Short delay for stable RSSI
        int rawRSSI = pClient->getRssi();
        addRSSI(rawRSSI);
        Serial.print("Initial RSSI: ");
        Serial.println(getMedianRSSI());
        
        // Check if we should auto-unlock on initial connection
        checkAutoLockUnlock();
    }

    return true;
}

// ===== Command Functions =====
void sendCommand(const String& command) {
    if (!connected || !pWriteCharacteristic) {
        Serial.println("⚠️ Not connected, can't send command");
        return;
    }

    pWriteCharacteristic->writeValue((uint8_t*)command.c_str(), command.length(), false);
    Serial.print("📤 Sent: ");
    Serial.println(command);
    
    // Update lock state and manual lock flag
    if (command == commandMap.at(Command::LOCK)) {
        lockStatus = LockStatus::LOCKED;
        isManualLock = true;
    } else if (command == commandMap.at(Command::UNLOCK)) {
        lockStatus = LockStatus::UNLOCKED;
        isManualLock = false;
    }
    feedWatchdog();
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

// ===== Sleep Functions =====
void setupPinsForWakeup() {
    for (const auto& pinCommand : pinToCommandMap) {
        pinMode(pinCommand.first, INPUT_PULLDOWN);
        esp_sleep_enable_ext0_wakeup((gpio_num_t)pinCommand.first, HIGH);
    }
}

void goToDeepSleep(uint32_t seconds) {
    Serial.printf("Sleeping for %d seconds...\n", seconds);
    esp_sleep_enable_timer_wakeup(seconds * 1000000);
    esp_deep_sleep_start();
}

// ===== Main Functions =====
void setup() {
    Serial.begin(115200);
    Serial.println("\n🚗 ESP32 Smart Car Key");
    
    // Initialize watchdog
    initWatchdog();
    feedWatchdog();
    
    // Initialize BLE
    BLEDevice::init("ESP32_CarKey");
    setupPinsForWakeup();

    // Check wakeup reason
    esp_sleep_wakeup_cause_t wakeUpSource = esp_sleep_get_wakeup_cause();
    
    if (wakeUpSource == ESP_SLEEP_WAKEUP_TIMER) {
        Serial.println("Woke by timer - reconnecting");
        feedWatchdog();
        if (!connectToServer()) {
            if (connectionRetries >= MAX_RAPID_RETRIES) {
                goToDeepSleep(LONG_SLEEP_DURATION_S);
            } else {
                goToDeepSleep(CONNECTION_RETRY_DELAY_MS / 1000);
            }
        }
    } else {
        Serial.print("Wakeup cause: ");
        if (wakeUpSource == ESP_SLEEP_WAKEUP_EXT0) {
            Serial.println("GPIO (button press)");
        } else {
            Serial.println("Power on/reset");
        }
        
        feedWatchdog();
        if (!connectToServer()) {
            goToDeepSleep(CONNECTION_RETRY_DELAY_MS / 1000);
        }
    }
}

void loop() {
    feedWatchdog();
    
    if (connected) {
        // Handle button presses
        Command cmd = getCommandFromInput();
        if (cmd != Command::UNKNOWN) {
            Serial.println("Command: " + commandMap.at(cmd));
            
            if (cmd == Command::RESET) {
                Serial.println("Resetting...");
                esp_restart();
            } else {
                sendCommand(commandMap.at(cmd));
            }
            delay(500);
        }

        // Periodic RSSI check
        if (millis() - lastRssiCheckTime > RSSI_UPDATE_INTERVAL) {
            lastRssiCheckTime = millis();
            if (pClient && pClient->isConnected()) {
                int rawRSSI = pClient->getRssi();
                addRSSI(rawRSSI);
                Serial.print("RSSI: ");
                Serial.print(rawRSSI);
                Serial.print(" (median: ");
                Serial.print(getMedianRSSI());
                Serial.println(")");
                
                checkAutoLockUnlock();
            }
        }

        delay(100);
    } else {
        Serial.println("Disconnected, resetting...");
        esp_restart();
    }
}
