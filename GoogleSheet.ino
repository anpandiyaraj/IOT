#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <WiFiClientSecure.h>

const char* ssid = "WIFIF Name";
const char* password = "Password";
const String scriptURL = "https://script.google.com/macros/s/SCRIPT_ID/exec";



// RFID pins
#define SS_PIN 15   // GPIO15 = D8
#define RST_PIN 4   // GPIO4 = D2

MFRC522 rfid(SS_PIN, RST_PIN);  // Create MFRC522 instance

void setup() {
  Serial.begin(9600);
  SPI.begin();         // Init SPI bus
  rfid.PCD_Init();     // Init MFRC522

  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" connected!");
  Serial.println("Scan an RFID tag...");
}

void loop() {
  // Look for new cards
  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial()) return;

  // Read UID
  String uidString = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uidString += "0";
    uidString += String(rfid.uid.uidByte[i], HEX);
  }
  uidString.toUpperCase();

  Serial.print("Card UID: ");
  Serial.println(uidString);

  // Send to Google Sheet
  sendToSheet(uidString, "login");  // or "logout"

  // Halt PICC and stop encryption
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  delay(2000);  // Prevent duplicate reads
}



void sendToSheet(String uid, String action) {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure client;
    client.setInsecure();  // ⚠️ Accept all certificates (not secure, but works for testing)

    HTTPClient http;
    String url = scriptURL + "?uid=" + uid + "&action=" + action;
    http.begin(client, url);  // Use secure client
    int httpCode = http.GET();
    String payload = http.getString();
    Serial.print("HTTP Response code: ");
    Serial.println(httpCode);
    Serial.print("Response: ");
    Serial.println(payload);
    http.end();
  } else {
    Serial.println("WiFi not connected");
  }
}
