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
#include <numeric>
#include <algorithm>

// ===== Configuration =====
const String serverMAC = "5C:01:3B:9B:90:DD"; // Replace with your server's MAC
const uint32_t blePasskey = 123456;

// UUIDs for the service and characteristics
#define SERVICE_UUID             "726f72c1-055d-4f94-b090-c1afeec24782"
#define CHARACTERISTIC_UUID_1    "c1cf0c5d-d07f-4f7c-ad2e-9cb3e49286b4" // Server response
#define CHARACTERISTIC_UUID_2    "b12523bb-5e18-41fa-a498-cceb16bb7628" // Client command

// GPIO Pins for Commands (Wake-up Sources)
#define LOCK_PIN               16
#define UNLOCK_PIN             17
#define TRUNK_PIN              4
#define LOCATE_PIN             2
#define ELIGHT_PIN             15
#define RESET_PIN              13  // Added for manual reset functionality

// Timing and thresholds
#define DEBOUNCE_DELAY_MS      50
#define RSSI_THRESHOLD_UNLOCK  -85  // Unlock when RSSI is better than -85
#define RSSI_THRESHOLD_LOCK    -86  // Lock when RSSI is worse than -86
#define RSSI_AVERAGE_WINDOW    5
#define CONNECTION_TIMEOUT_MS  10000
#define CONNECTION_RETRY_DELAY_MS 5000
#define MAX_RAPID_RETRIES      3
#define LONG_SLEEP_DURATION_S  60  // Sleep for 1 minute after max retries
#define WATCHDOG_TIMEOUT_MS    30000  // Watchdog timeout in milliseconds (30s)
#define RSSI_UPDATE_INTERVAL   2000 // Check RSSI every 2 seconds

// Command enumeration
enum class Command { LOCK, UNLOCK, TRUNK, LOCATE, ELIGHT, RESET, UNKNOWN };

// Lock status enumeration
enum class LockStatus { NONE, LOCKED, UNLOCKED };

// Pin to command mapping
const std::map<int, Command> pinToCommandMap = {
    {LOCK_PIN,   Command::LOCK},
    {UNLOCK_PIN, Command::UNLOCK},
    {TRUNK_PIN,  Command::TRUNK},
    {LOCATE_PIN, Command::LOCATE},
    {ELIGHT_PIN, Command::ELIGHT},
    {RESET_PIN,  Command::RESET}  // Added reset command
};

// Command to string mapping
const std::map<Command, String> commandMap = {
    {Command::LOCK,   "LOCK"},
    {Command::UNLOCK, "UNLOCK"},
    {Command::TRUNK,  "TRUNK"},
    {Command::LOCATE, "LOCATE"},
    {Command::ELIGHT, "ELIGHT"},
    {Command::RESET,  "RESET"}
};

// Global variables
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pCharacteristic_1 = nullptr;
BLERemoteCharacteristic* pCharacteristic_2 = nullptr;
bool connected = false;
std::vector<int> rssiBuffer;
LockStatus lockStatus = LockStatus::NONE;
int connectionRetries = 0;
unsigned long lastRssiCheckTime = 0;

// ===== Watchdog Functions =====
void initWatchdog() {
    // Initialize Task Watchdog Timer (TWDT)
    esp_task_wdt_config_t twdt_config = {
        .timeout_ms = WATCHDOG_TIMEOUT_MS,
        .idle_core_mask = (1 << 0) // Monitor Core 0 (where Arduino runs)
    };
    
    if (esp_task_wdt_init(&twdt_config) != ESP_OK) {
        Serial.println("Failed to initialize watchdog");
    }
    
    // Subscribe this task to TWDT
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
        rssiBuffer.push_back(rssi);
        if (rssiBuffer.size() > RSSI_AVERAGE_WINDOW) {
            rssiBuffer.erase(rssiBuffer.begin());
        }
    }
}

int getAverageRSSI() {
    if (rssiBuffer.empty()) return -127;
    int sum = std::accumulate(rssiBuffer.begin(), rssiBuffer.end(), 0);
    return sum / rssiBuffer.size();
}

// ===== Auto Lock/Unlock Functions =====
void checkAutoLockUnlock() {
    if (!connected || !pClient->isConnected()) {
        return;
    }

    int currentRSSI = getAverageRSSI();
    Serial.print("Checking auto lock/unlock. RSSI: ");
    Serial.print(currentRSSI);
    Serial.print(", Lock status: ");
    switch(lockStatus) {
        case LockStatus::NONE: Serial.println("NONE"); break;
        case LockStatus::LOCKED: Serial.println("LOCKED"); break;
        case LockStatus::UNLOCKED: Serial.println("UNLOCKED"); break;
    }

    // Auto-unlock logic (when RSSI is strong and car is locked or status unknown)
    if (currentRSSI > RSSI_THRESHOLD_UNLOCK && 
        (lockStatus == LockStatus::NONE || lockStatus == LockStatus::LOCKED)) {
        Serial.println("Auto UNLOCK triggered");
        sendCommand(commandMap.at(Command::UNLOCK));
        lockStatus = LockStatus::UNLOCKED;
    }
    // Auto-lock logic (when RSSI is weak and car is unlocked)
    else if (currentRSSI < RSSI_THRESHOLD_LOCK && lockStatus == LockStatus::UNLOCKED) {
        Serial.println("Auto LOCK triggered");
        sendCommand(commandMap.at(Command::LOCK));
        lockStatus = LockStatus::LOCKED;
    }
}

// ===== BLE Functions =====
void notifyCallback(BLERemoteCharacteristic* pBLERemoteCharacteristic, 
                   uint8_t* pData, size_t length, bool isNotify) {
    Serial.print("Notification received: ");
    for (int i = 0; i < length; i++) {
        Serial.print((char)pData[i]);
    }
    Serial.println();
}

class ClientSecurityCallbacks : public BLESecurityCallbacks {
    bool onSecurityRequest() {
        Serial.println("Security request received");
        return true;
    }

    bool onConfirmPIN(uint32_t pass_key) {
        Serial.print("Confirm passkey: ");
        Serial.println(pass_key);
        return true;
    }

    uint32_t onPassKeyRequest() {
        Serial.println("Passkey requested");
        return blePasskey;
    }

    void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) {
        if (cmpl.success) {
            Serial.println("✅ Authentication success");
            feedWatchdog();
            
            // After authentication, check if we should auto-unlock
            if (connected && getAverageRSSI() > RSSI_THRESHOLD_UNLOCK && 
                (lockStatus == LockStatus::NONE || lockStatus == LockStatus::LOCKED)) {
                Serial.println("Auto UNLOCK after authentication");
                sendCommand(commandMap.at(Command::UNLOCK));
                lockStatus = LockStatus::UNLOCKED;
            }
        } else {
            Serial.println("❌ Authentication failed");
        }
    }

    void onPassKeyNotify(uint32_t pass_key) {
        Serial.print("Passkey notify: ");
        Serial.println(pass_key);
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
        lockStatus = LockStatus::NONE; // Reset status on disconnect
        Serial.println("🔌 Disconnected from server");

        // Clean up BLE resources
        if (pCharacteristic_1) pCharacteristic_1 = nullptr;
        if (pCharacteristic_2) pCharacteristic_2 = nullptr;
        if (pClient) {
            pClient->disconnect();
            delete pClient;
            pClient = nullptr;
        }

        // Prepare for reconnection
        if (connectionRetries < MAX_RAPID_RETRIES) {
            Serial.printf("Will retry in %dms (attempt %d/%d)\n", 
                         CONNECTION_RETRY_DELAY_MS, 
                         connectionRetries + 1, 
                         MAX_RAPID_RETRIES);
            esp_sleep_enable_timer_wakeup(CONNECTION_RETRY_DELAY_MS * 1000);
        } else {
            Serial.printf("Max retries reached, sleeping for %d seconds\n", LONG_SLEEP_DURATION_S);
            esp_sleep_enable_timer_wakeup(LONG_SLEEP_DURATION_S * 1000000);
        }
        esp_deep_sleep_start();
    }
};

bool connectToServer() {
    Serial.print("Connecting to server at ");
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

    // Attempt connection with manual timeout
    unsigned long connectStart = millis();
    bool connectionSuccess = pClient->connect(bleServerAddress);
    
    // Manual timeout check
    while (!connectionSuccess && (millis() - connectStart < CONNECTION_TIMEOUT_MS)) {
        delay(100);
        feedWatchdog();
        connectionSuccess = pClient->isConnected();
    }

    if (!connectionSuccess) {
        Serial.println("❌ Failed to connect to server");
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
    pCharacteristic_1 = pRemoteService->getCharacteristic(CHARACTERISTIC_UUID_1);
    pCharacteristic_2 = pRemoteService->getCharacteristic(CHARACTERISTIC_UUID_2);

    if (!pCharacteristic_1 || !pCharacteristic_2) {
        Serial.println("❌ Failed to find characteristics");
        pClient->disconnect();
        connected = false;
        return false;
    }

    // Register for notifications if available
    if (pCharacteristic_1->canNotify()) {
        pCharacteristic_1->registerForNotify(notifyCallback);
        Serial.println("🔔 Notification callback registered");
    }

    if (pCharacteristic_2->canWrite()) {
        Serial.println("✉️ Ready to send commands");
    }

    // Get initial RSSI
    if (pClient->isConnected()) {
        delay(100); // Short delay for stable RSSI
        int rawRSSI = pClient->getRssi();
        addRSSI(rawRSSI);
        Serial.print("Initial RSSI: ");
        Serial.println(getAverageRSSI());
        
        // Check if we should auto-unlock on initial connection
        checkAutoLockUnlock();
    }

    return true;
}

// ===== Command Functions =====
void sendCommand(const String& command) {
    if (!connected || !pCharacteristic_2) {
        Serial.println("⚠️ Cannot send command: not connected");
        return;
    }

    pCharacteristic_2->writeValue((uint8_t*)command.c_str(), command.length(), false);
    Serial.print("📤 Sent command: ");
    Serial.println(command);
    
    // Update lock state
    if (command == commandMap.at(Command::LOCK)) {
        lockStatus = LockStatus::LOCKED;
    } else if (command == commandMap.at(Command::UNLOCK)) {
        lockStatus = LockStatus::UNLOCKED;
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
    Serial.printf("Going to deep sleep for %d seconds...\n", seconds);
    esp_sleep_enable_timer_wakeup(seconds * 1000000);
    esp_deep_sleep_start();
}

// ===== Main Functions =====
void setup() {
    Serial.begin(115200);
    Serial.println("\n🚀 Smart Car Key - Enhanced Version");
    
    // Initialize watchdog
    initWatchdog();
    feedWatchdog();
    
    // Initialize BLE
    BLEDevice::init("ESP32_CarKey");
    setupPinsForWakeup();

    // Check wakeup reason
    esp_sleep_wakeup_cause_t wakeUpSource = esp_sleep_get_wakeup_cause();
    
    if (wakeUpSource == ESP_SLEEP_WAKEUP_TIMER) {
        Serial.println("Woke up by timer - attempting to reconnect");
        feedWatchdog();
        if (!connectToServer()) {
            if (connectionRetries >= MAX_RAPID_RETRIES) {
                Serial.println("Max retries reached, going to long sleep");
                goToDeepSleep(LONG_SLEEP_DURATION_S);
            } else {
                Serial.println("Reconnection failed, going back to sleep");
                goToDeepSleep(CONNECTION_RETRY_DELAY_MS / 1000);
            }
        }
    } else {
        Serial.print("Woke up by: ");
        if (wakeUpSource == ESP_SLEEP_WAKEUP_EXT0) {
            Serial.println("GPIO (button press)");
        } else {
            Serial.println("Power on/reset");
        }
        
        feedWatchdog();
        if (!connectToServer()) {
            Serial.println("Initial connection failed, going to sleep for retry");
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
            Serial.println("Command detected: " + commandMap.at(cmd));
            
            if (cmd == Command::RESET) {
                Serial.println("Manual reset triggered");
                esp_restart();
            } else {
                sendCommand(commandMap.at(cmd));
            }
            delay(500);
        }

        // Periodically check RSSI and auto lock/unlock
        if (millis() - lastRssiCheckTime > RSSI_UPDATE_INTERVAL) {
            lastRssiCheckTime = millis();
            if (pClient && pClient->isConnected()) {
                int rawRSSI = pClient->getRssi();
                addRSSI(rawRSSI);
                Serial.print("Current RSSI: ");
                Serial.print(rawRSSI);
                Serial.print(" (avg: ");
                Serial.print(getAverageRSSI());
                Serial.println(")");
                
                // Check if we should auto lock/unlock
                checkAutoLockUnlock();
            }
        }

        delay(100);
    } else {
        Serial.println("Unexpected disconnected state, resetting");
        esp_restart();
    }
}
