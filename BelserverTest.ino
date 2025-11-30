#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "esp_gap_ble_api.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_sleep.h"
#include <map>
#include <vector>
#include <string>
#include <Preferences.h>
#include "mbedtls/sha256.h"
#include "esp_system.h"

// Disconnection reasons
enum class DisconnectReason { NORMAL, AUTH_FAILURE };
DisconnectReason lastDisconnectReason = DisconnectReason::NORMAL;

// BLE Objects
BLEServer* pServer = NULL;
BLECharacteristic* pCharacteristic_1 = NULL;
BLECharacteristic* pCharacteristic_2 = NULL;
BLEDescriptor* pDescr_1;
BLE2902* pBLE2902_1;
BLE2902* pBLE2902_2;

// Connection state
bool deviceConnected = false;
bool oldDeviceConnected = false;
bool isAuthenticated = false;            // application-layer + BLE-layer authentication
bool bleAuthenticated = false;           // BLE pairing/auth success
uint16_t currentConnId = 0;
unsigned long lastActivityTime = 0;
bool initialBoot = true;
unsigned long bootTime = 0;

// New variable to track lock state
bool isLocked = false;

// Configuration
const unsigned long WAKEUP_INTERVAL = 10000000;   // 10 seconds
const unsigned long WAKEUP_DELAY = 1000;          // 1 second delay after wakeup
const unsigned long CONNECTION_TIMEOUT = 2000;    // 2 seconds to find connection
const unsigned long INITIAL_BOOT_DELAY = 120000;  // 2 minutes initial awake time

// Security: keep BLE static passkey only as transport-level, but also require app layer HMAC
const String PASSKEY = "123456";
const unsigned long DISCONNECT_LOCK_DELAY = 1000;  // 1 second delay before auto-lock

// Application-layer shared secret for HMAC (should be provisioned securely in production)
const char* APP_SECRET = "SuperSecretAppKey_ReplaceThis"; // replace in production

// Lockout / rate limiting
const int MAX_FAILED_ATTEMPTS = 5;
const unsigned long LOCKOUT_DURATION_MS = 5 * 60 * 1000UL; // 5 minutes

// UUIDs
#define SERVICE_UUID "726f72c1-055d-4f94-b090-c1afeec24782"
#define CHARACTERISTIC_UUID_1 "c1cf0c5d-d07f-4f7c-ad2e-9cb3e49286b4"  // Server response / challenge
#define CHARACTERISTIC_UUID_2 "b12523bb-5e18-41fa-a498-cceb16bb7628"  // Client commands / responses

// Pin Definitions
enum class Pin : uint8_t {
  LOCK = 12,
  UNLOCK = 13,
  TRUNK = 18,
  ELIGHT = 25,
  LOCATE = 19
};

// Command Definitions
enum class Command {
  LOCK,
  UNLOCK,
  TRUNK,
  LOCATE,
  ELIGHT,
  UNKNOWN
};

const std::map<String, String> COMMAND_RESPONSES = {
  { "LOCK", "Door Locked" },
  { "UNLOCK", "Door Unlocked" },
  { "LOCATE", "Located" },
  { "TRUNK", "Trunk Released" },
  { "ELIGHT", "Emergency Light" }
};

// Whitelist, lockout, challenge tracking
Preferences preferences;
std::vector<String> whitelist; // stored as MAC strings "AA:BB:CC:DD:EE:FF"
std::map<String, int> failedAttempts; // per-MAC
std::map<String, unsigned long> lockoutUntil; // per-MAC timestamp millis()
std::map<String, std::vector<uint8_t>> challenges; // per-MAC nonce awaiting response

// Track last seen remote address (updated in GAP callbacks)
String lastRemoteAddr = "";

// Forward declaration
void performLock();
String macToString(uint8_t* bda);
void loadWhitelist();
void saveWhitelist();
bool isWhitelisted(const String& mac);
void addToWhitelist(const String& mac);
void removeFromWhitelist(const String& mac);
String toHex(const uint8_t* data, size_t len);
std::vector<uint8_t> generateNonce(size_t len = 16);
void compute_hmac_sha256(const uint8_t* key, size_t keylen,
                         const uint8_t* data, size_t datalen,
                         uint8_t out[32]);

// ==================== GAP Callback ====================
void gapCallback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
  switch (event) {
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
      {
        char macStr[18];
        sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
                param->update_conn_params.bda[0], param->update_conn_params.bda[1],
                param->update_conn_params.bda[2], param->update_conn_params.bda[3],
                param->update_conn_params.bda[4], param->update_conn_params.bda[5]);
        lastRemoteAddr = String(macStr);
        Serial.print("Device connected (GAP update): ");
        Serial.println(lastRemoteAddr);

        // If device is currently locked out, disconnect immediately
        auto itLock = lockoutUntil.find(lastRemoteAddr);
        if (itLock != lockoutUntil.end()) {
          if (millis() < itLock->second) {
            Serial.printf("Device %s is locked out until %lu - disconnecting\n", lastRemoteAddr.c_str(), itLock->second);
            if (pServer != nullptr && currentConnId > 0) {
              pServer->disconnect(currentConnId);
            }
            return;
          } else {
            // lockout expired
            lockoutUntil.erase(itLock);
            failedAttempts[lastRemoteAddr] = 0;
          }
        }
        break;
      }
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
      {
        char macStr[18];
        sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
                param->ble_security.auth_cmpl.bd_addr[0], param->ble_security.auth_cmpl.bd_addr[1],
                param->ble_security.auth_cmpl.bd_addr[2], param->ble_security.auth_cmpl.bd_addr[3],
                param->ble_security.auth_cmpl.bd_addr[4], param->ble_security.auth_cmpl.bd_addr[5]);
        lastRemoteAddr = String(macStr);

        if (param->ble_security.auth_cmpl.success) {
          Serial.println("BLE pairing/authentication successful (GAP)");
          bleAuthenticated = true;
          failedAttempts[lastRemoteAddr] = 0; // reset failures on BLE success

          // If device is whitelisted already, mark full authenticated once app-layer also validated
          if (isWhitelisted(lastRemoteAddr)) {
            // generate a new application-layer challenge and send
            std::vector<uint8_t> nonce = generateNonce(16);
            challenges[lastRemoteAddr] = nonce;
            String challengeMsg = "CHALLENGE:" + toHex(nonce.data(), nonce.size());
            pCharacteristic_1->setValue(challengeMsg.c_str());
            pCharacteristic_1->notify();
            Serial.println("Sent challenge to whitelisted device: " + lastRemoteAddr);
          } else {
            // Not in whitelist: still allow to complete pairing, but require app-layer verification to add to whitelist
            std::vector<uint8_t> nonce = generateNonce(16);
            challenges[lastRemoteAddr] = nonce;
            String challengeMsg = "CHALLENGE:" + toHex(nonce.data(), nonce.size());
            pCharacteristic_1->setValue(challengeMsg.c_str());
            pCharacteristic_1->notify();
            Serial.println("Sent challenge to new device (will be added to whitelist after app-layer verification): " + lastRemoteAddr);
          }
        } else {
          Serial.println("Authentication failed - disconnecting (GAP)");
          bleAuthenticated = false;
          lastDisconnectReason = DisconnectReason::AUTH_FAILURE;
          // increment failed attempts for this MAC
          int &fa = failedAttempts[lastRemoteAddr];
          fa++;
          if (fa >= MAX_FAILED_ATTEMPTS) {
            lockoutUntil[lastRemoteAddr] = millis() + LOCKOUT_DURATION_MS;
            Serial.printf("Device %s locked out for %lu ms due to repeated failures\n", lastRemoteAddr.c_str(), LOCKOUT_DURATION_MS);
          }
          if (pServer != nullptr && currentConnId > 0) {
            pServer->disconnect(currentConnId);
          }
        }
      }
      break;
    default:
      break;
  }
}

// ==================== Server Callbacks ====================
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
    currentConnId = pServer->getConnId();
    // We'll wait for both BLE auth and app-layer auth
    isAuthenticated = false;
    bleAuthenticated = false;
    lastDisconnectReason = DisconnectReason::NORMAL;
    Serial.println("Client connected - waiting for authentication (both BLE and app-layer)");
  };

  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    currentConnId = 0;
    isAuthenticated = false;
    bleAuthenticated = false;
    lastActivityTime = millis();
    Serial.println("Device disconnected");
    BLEDevice::startAdvertising();

    // Add logic to automatically lock if not already locked (normal disconnect)
    if (lastDisconnectReason == DisconnectReason::NORMAL) {
      if (!isLocked) {
        Serial.println("Device disconnected - performing automatic lock");
        delay(DISCONNECT_LOCK_DELAY);  // Optional delay before locking
        performLock();
      }
    }
  }
};

// ==================== Characteristic Callbacks ====================
class CharacteristicCallBack : public BLECharacteristicCallbacks {
private:
  void controlPin(Pin pin, float seconds) {
    digitalWrite(static_cast<uint8_t>(pin), HIGH);
    delay(static_cast<int>(seconds * 1000));
    digitalWrite(static_cast<uint8_t>(pin), LOW);
  }

  void blinkPin(Pin pin, float seconds) {
    unsigned long endTime = millis() + static_cast<unsigned long>(seconds * 1000);
    while (millis() < endTime) {
      digitalWrite(static_cast<uint8_t>(pin), HIGH);
      delay(300);
      digitalWrite(static_cast<uint8_t>(pin), LOW);
      delay(200);
    }
  }

public:
  Command parseCommand(const String& command) {
    String cmd = command;
    cmd.toUpperCase();

    if (cmd == "LOCK") return Command::LOCK;
    if (cmd == "UNLOCK") return Command::UNLOCK;
    if (cmd == "TRUNK") return Command::TRUNK;
    if (cmd == "LOCATE") return Command::LOCATE;
    if (cmd == "ELIGHT") return Command::ELIGHT;
    return Command::UNKNOWN;
  }

  void onWrite(BLECharacteristic* pChar) override {
    String receivedValue = String((char*)pChar->getValue().c_str());
    Serial.println("Received raw write: " + receivedValue);

    // If device is in lockout, ignore
    String remote = lastRemoteAddr;
    auto itLock = lockoutUntil.find(remote);
    if (itLock != lockoutUntil.end()) {
      if (millis() < itLock->second) {
        Serial.println("Device is locked out - ignoring write");
        return;
      } else {
        lockoutUntil.erase(itLock);
        failedAttempts[remote] = 0;
      }
    }

    // Handle application-layer response for challenge before accepting commands
    if (receivedValue.startsWith("RESP:")) {
      String respHex = receivedValue.substring(5);
      // Get stored challenge
      auto it = challenges.find(remote);
      if (it == challenges.end()) {
        Serial.println("No challenge found for device - rejecting response");
        // increment failure count
        int &fa = failedAttempts[remote];
        fa++;
        if (fa >= MAX_FAILED_ATTEMPTS) {
          lockoutUntil[remote] = millis() + LOCKOUT_DURATION_MS;
          Serial.printf("Device %s locked out for %lu ms due to repeated failures (no challenge)\n", remote.c_str(), LOCKOUT_DURATION_MS);
        }
        return;
      }
      // compute expected HMAC
      std::vector<uint8_t>& nonce = it->second;
      uint8_t expectedHmac[32];
      compute_hmac_sha256((const uint8_t*)APP_SECRET, strlen(APP_SECRET),
                          nonce.data(), nonce.size(), expectedHmac);
      String expectedHex = toHex(expectedHmac, 32);
      if (expectedHex.equalsIgnoreCase(respHex)) {
        Serial.println("Application-layer authentication successful");
        isAuthenticated = true;
        // add to whitelist after successful app-layer auth + BLE pairing
        if (bleAuthenticated) {
          addToWhitelist(remote);
          Serial.println("Device added to whitelist: " + remote);
        } else {
          Serial.println("BLE pairing not completed yet; will add to whitelist once BLE auth completes.");
        }
        // clear challenge
        challenges.erase(it);
        // send acknowledgment
        pCharacteristic_1->setValue("AUTH_OK");
        pCharacteristic_1->notify();
      } else {
        Serial.println("Application-layer authentication failed (bad HMAC)");
        int &fa = failedAttempts[remote];
        fa++;
        if (fa >= MAX_FAILED_ATTEMPTS) {
          lockoutUntil[remote] = millis() + LOCKOUT_DURATION_MS;
          Serial.printf("Device %s locked out for %lu ms due to repeated HMAC failures\n", remote.c_str(), LOCKOUT_DURATION_MS);
        }
        pCharacteristic_1->setValue("AUTH_FAIL");
        pCharacteristic_1->notify();
      }
      return;
    }

    // Only accept control commands (LOCK/UNLOCK/...) after both BLE auth and app-layer auth
    if (!bleAuthenticated || !isAuthenticated) {
      Serial.println("Unauthorized access attempt - ignoring write (needs BLE + app auth)");
      // Help client by sending challenge if BLE authenticated but app auth missing
      if (bleAuthenticated && !isAuthenticated) {
        auto itc = challenges.find(remote);
        if (itc != challenges.end()) {
          String challengeMsg = "CHALLENGE:" + toHex(itc->second.data(), itc->second.size());
          pCharacteristic_1->setValue(challengeMsg.c_str());
          pCharacteristic_1->notify();
          Serial.println("Re-sent challenge to client");
        }
      }
      return;
    }

    Command cmd = parseCommand(receivedValue);
    String response;

    auto it = COMMAND_RESPONSES.find(receivedValue);
    if (it != COMMAND_RESPONSES.end()) {
      response = it->second;
      Serial.println("Executing command: " + receivedValue);

      switch (cmd) {
        case Command::LOCK:
          controlPin(Pin::LOCK, 0.5);
          isLocked = true;  // Update lock state
          break;
        case Command::UNLOCK:
          controlPin(Pin::UNLOCK, 0.5);
          isLocked = false;  // Update lock state
          break;
        case Command::TRUNK:
          controlPin(Pin::TRUNK, 1.0);
          break;
        case Command::LOCATE:
          blinkPin(Pin::LOCATE, 3.0);
          break;
        case Command::ELIGHT:
          controlPin(Pin::ELIGHT, 30);
          break;
        default:
          break;
      }
    } else {
      response = "INVALID_COMMAND";
      Serial.println("Unknown command received");
    }

    pCharacteristic_1->setValue(response.c_str());
    pCharacteristic_1->notify();
    Serial.println("Response sent: " + response);
  }

  void onRead(BLECharacteristic* pChar) override {
    if (!isAuthenticated) {
      Serial.println("Unauthorized access attempt - blocking read");
      pChar->setValue("");
      return;
    }
  }
};

// ==================== Security Callbacks ====================
class MySecurityCallbacks : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override {
    Serial.println("Passkey requested");
    return atoi(PASSKEY.c_str());
  }

  void onPassKeyNotify(uint32_t pass_key) override {
    Serial.printf("Passkey Notify: %d\n", pass_key);
  }

  bool onConfirmPIN(uint32_t pass_key) override {
    Serial.printf("Confirming passkey: %d\n", pass_key);
    bool match = (pass_key == atoi(PASSKEY.c_str()));
    if (!match) {
      Serial.println("Passkey mismatch - rejecting");
      bleAuthenticated = false;
      int &fa = failedAttempts[lastRemoteAddr];
      fa++;
      if (fa >= MAX_FAILED_ATTEMPTS) {
        lockoutUntil[lastRemoteAddr] = millis() + LOCKOUT_DURATION_MS;
        Serial.printf("Device %s locked out for %lu ms due to repeated PIN mismatches\n", lastRemoteAddr.c_str(), LOCKOUT_DURATION_MS);
      }
      if (pServer != nullptr && currentConnId > 0) {
        pServer->disconnect(currentConnId);
      }
    }
    return match;
  }

  void onAuthenticationComplete(esp_ble_auth_cmpl_t auth_cmpl) override {
    char macStr[18];
    sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
            auth_cmpl.bd_addr[0], auth_cmpl.bd_addr[1],
            auth_cmpl.bd_addr[2], auth_cmpl.bd_addr[3],
            auth_cmpl.bd_addr[4], auth_cmpl.bd_addr[5]);
    lastRemoteAddr = String(macStr);

    if (auth_cmpl.success) {
      Serial.println("Authentication successful (security callback)");
      bleAuthenticated = true;
      failedAttempts[lastRemoteAddr] = 0;

      // send application-layer challenge
      std::vector<uint8_t> nonce = generateNonce(16);
      challenges[lastRemoteAddr] = nonce;
      String challengeMsg = "CHALLENGE:" + toHex(nonce.data(), nonce.size());
      pCharacteristic_1->setValue(challengeMsg.c_str());
      pCharacteristic_1->notify();
    } else {
      Serial.println("Authentication failed - disconnecting (security callback)");
      bleAuthenticated = false;
      int &fa = failedAttempts[lastRemoteAddr];
      fa++;
      if (fa >= MAX_FAILED_ATTEMPTS) {
        lockoutUntil[lastRemoteAddr] = millis() + LOCKOUT_DURATION_MS;
        Serial.printf("Device %s locked out for %lu ms due to repeated auth failures\n", lastRemoteAddr.c_str(), LOCKOUT_DURATION_MS);
      }
      if (pServer != nullptr && currentConnId > 0) {
        pServer->disconnect(currentConnId);
      }
    }
  }

  bool onSecurityRequest() override {
    Serial.println("Security request received - requiring authentication");
    return true;
  }
};

// ==================== Helper function to perform the lock action
void performLock() {
  Serial.println("Performing lock action due to disconnection");
  digitalWrite(static_cast<uint8_t>(Pin::LOCK), HIGH);
  delay(500);  // Simulate lock activation time
  digitalWrite(static_cast<uint8_t>(Pin::LOCK), LOW);
  isLocked = true;
}

// ==================== Utility functions ====================
String macToString(uint8_t* bda) {
  char macStr[18];
  sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
          bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
  return String(macStr);
}

void loadWhitelist() {
  whitelist.clear();
  preferences.begin("ble_lock", true);
  String csv = preferences.getString("whitelist", "");
  preferences.end();
  if (csv.length() == 0) return;
  int start = 0;
  while (start < (int)csv.length()) {
    int idx = csv.indexOf(',', start);
    if (idx == -1) idx = csv.length();
    String mac = csv.substring(start, idx);
    mac.trim();
    if (mac.length() > 0) whitelist.push_back(mac);
    start = idx + 1;
  }
  Serial.print("Loaded whitelist: ");
  for (auto &m : whitelist) Serial.print(m + " ");
  Serial.println();
}

void saveWhitelist() {
  String csv = "";
  for (size_t i = 0; i < whitelist.size(); ++i) {
    csv += whitelist[i];
    if (i + 1 < whitelist.size()) csv += ",";
  }
  preferences.begin("ble_lock", false);
  preferences.putString("whitelist", csv);
  preferences.end();
  Serial.println("Whitelist saved");
}

bool isWhitelisted(const String& mac) {
  for (auto &m : whitelist) {
    if (m.equalsIgnoreCase(mac)) return true;
  }
  return false;
}

void addToWhitelist(const String& mac) {
  if (!isWhitelisted(mac)) {
    whitelist.push_back(mac);
    saveWhitelist();
  }
}

void removeFromWhitelist(const String& mac) {
  for (auto it = whitelist.begin(); it != whitelist.end(); ++it) {
    if (it->equalsIgnoreCase(mac)) {
      whitelist.erase(it);
      saveWhitelist();
      return;
    }
  }
}

String toHex(const uint8_t* data, size_t len) {
  String s = "";
  const char hexmap[] = "0123456789ABCDEF";
  for (size_t i = 0; i < len; ++i) {
    uint8_t v = data[i];
    s += hexmap[(v >> 4) & 0xF];
    s += hexmap[v & 0xF];
  }
  return s;
}

std::vector<uint8_t> generateNonce(size_t len) {
  std::vector<uint8_t> nonce(len);
  for (size_t i = 0; i < len; ++i) {
    uint32_t r = esp_random();
    nonce[i] = (uint8_t)(r & 0xFF);
  }
  return nonce;
}

void compute_hmac_sha256(const uint8_t* key, size_t keylen,
                         const uint8_t* data, size_t datalen,
                         uint8_t out[32]) {
  const size_t blockSize = 64;
  uint8_t keyBlock[blockSize];
  memset(keyBlock, 0, blockSize);
  if (keylen > blockSize) {
    // key = SHA256(key)
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, key, keylen);
    mbedtls_sha256_finish(&ctx, keyBlock);
    mbedtls_sha256_free(&ctx);
  } else {
    memcpy(keyBlock, key, keylen);
  }
  uint8_t o_key_pad[blockSize];
  uint8_t i_key_pad[blockSize];
  for (size_t i = 0; i < blockSize; ++i) {
    o_key_pad[i] = keyBlock[i] ^ 0x5c;
    i_key_pad[i] = keyBlock[i] ^ 0x36;
  }
  // inner hash = SHA256(i_key_pad || data)
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, i_key_pad, blockSize);
  mbedtls_sha256_update(&ctx, data, datalen);
  uint8_t innerHash[32];
  mbedtls_sha256_finish(&ctx, innerHash);
  mbedtls_sha256_free(&ctx);

  // outer hash = SHA256(o_key_pad || innerHash)
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, o_key_pad, blockSize);
  mbedtls_sha256_update(&ctx, innerHash, 32);
  mbedtls_sha256_finish(&ctx, out);
  mbedtls_sha256_free(&ctx);
}

// ==================== Setup ====================
void setup() {
  Serial.begin(115200);
  Serial.println("Initializing BLE Server (secured with pairing, whitelist, rate limiting and app-layer HMAC)");

  bootTime = millis();

  // Load whitelist from NVS
  loadWhitelist();

  // Initialize GPIO pins
  pinMode(static_cast<uint8_t>(Pin::LOCK), OUTPUT);
  pinMode(static_cast<uint8_t>(Pin::UNLOCK), OUTPUT);
  pinMode(static_cast<uint8_t>(Pin::TRUNK), OUTPUT);
  pinMode(static_cast<uint8_t>(Pin::LOCATE), OUTPUT);
  pinMode(static_cast<uint8_t>(Pin::ELIGHT), OUTPUT);

  // Set initial pin states (assuming unlocked at boot)
  digitalWrite(static_cast<uint8_t>(Pin::LOCK), LOW);
  digitalWrite(static_cast<uint8_t>(Pin::UNLOCK), LOW);
  digitalWrite(static_cast<uint8_t>(Pin::TRUNK), LOW);
  digitalWrite(static_cast<uint8_t>(Pin::LOCATE), LOW);
  digitalWrite(static_cast<uint8_t>(Pin::ELIGHT), LOW);

  // Initialize lock state
  isLocked = false;

  // Initialize BLE
  BLEDevice::init("ESP32_Lock");
  esp_ble_gap_register_callback(gapCallback);

  // Configure BLE Security - require LE Secure Connections, MITM, bonding
  BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT_MITM);
  BLEDevice::setSecurityCallbacks(new MySecurityCallbacks());

  BLESecurity* pSecurity = new BLESecurity();
  pSecurity->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
  pSecurity->setCapability(ESP_IO_CAP_OUT); // device displays passkey (we return static); change if using other IO
  pSecurity->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  pSecurity->setKeySize(16);

  // Set static passkey (transport-level); consider removing in production or rotating dynamically
  uint32_t passkey = atoi(PASSKEY.c_str());
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_STATIC_PASSKEY, &passkey, sizeof(uint32_t));

  // Create BLE Server
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  // Create BLE Service
  BLEService* pService = pServer->createService(SERVICE_UUID);

  // Create Characteristics
  pCharacteristic_1 = pService->createCharacteristic(
    CHARACTERISTIC_UUID_1,
    BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ);
  pCharacteristic_1->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED);

  pCharacteristic_2 = pService->createCharacteristic(
    CHARACTERISTIC_UUID_2,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY);
  pCharacteristic_2->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED);

  // Create Descriptors
  pDescr_1 = new BLEDescriptor((uint16_t)0x2901);
  pDescr_1->setValue("BLE Communication");
  pCharacteristic_1->addDescriptor(pDescr_1);

  pBLE2902_1 = new BLE2902();
  pBLE2902_1->setNotifications(true);
  pCharacteristic_1->addDescriptor(pBLE2902_1);

  pBLE2902_2 = new BLE2902();
  pBLE2902_2->setNotifications(true);
  pCharacteristic_2->addDescriptor(pBLE2902_2);

  // Set Callbacks
  pCharacteristic_1->setCallbacks(new CharacteristicCallBack());
  pCharacteristic_2->setCallbacks(new CharacteristicCallBack());

  // Start Service
  pService->start();

  // Start Advertising
  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);  // Functions that help with iPhone connections issue
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  Serial.println("Waiting for a client connection...");

  lastActivityTime = millis();
  delay(WAKEUP_DELAY);
}

// ==================== Main Loop ====================
void loop() {
  unsigned long currentTime = millis();

  // Handle initial boot period
  if (initialBoot) {
    if (currentTime - bootTime < INITIAL_BOOT_DELAY) {
      // Still in initial boot period - stay awake regardless of connection
      if (!deviceConnected && (currentTime - lastActivityTime > 10000)) {
        // Restart advertising every 10 seconds during initial boot period
        BLEDevice::startAdvertising();
        Serial.println("Advertising during initial boot period");
        lastActivityTime = currentTime;
      }
      delay(10);
      return;  // Skip normal sleep logic during initial boot
    } else {
      // Initial boot period ended
      initialBoot = false;
      lastActivityTime = currentTime;  // Reset timer for normal operation
      Serial.println("Initial boot period ended - entering normal operation");
    }
  }

  // Track connection state changes
  if (!deviceConnected && oldDeviceConnected) {
    delay(500);  // Give the bluetooth stack time to cleanup
    oldDeviceConnected = deviceConnected;
    BLEDevice::startAdvertising();
    Serial.println("Advertising restarted");
    lastActivityTime = currentTime;  // Reset timer when starting advertising
  }

  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
    lastActivityTime = currentTime;  // Reset timer when device connects
  }

  // Normal operation sleep logic (after initial boot period)
  if (!initialBoot && !deviceConnected && (currentTime - lastActivityTime > CONNECTION_TIMEOUT)) {
    Serial.println("No connection established - entering deep sleep");
    esp_sleep_enable_timer_wakeup(WAKEUP_INTERVAL);
    esp_deep_sleep_start();
  }

  // Periodically prune expired lockouts
  for (auto it = lockoutUntil.begin(); it != lockoutUntil.end();) {
    if (millis() > it->second) {
      Serial.println("Lockout expired for " + it->first);
      it = lockoutUntil.erase(it);
    } else {
      ++it;
    }
  }

  delay(10);  // Prevent watchdog issues
}
