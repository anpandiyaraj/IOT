#include <Keypad.h>

#define ROW_NUM   4 // four rows
#define COLUMN_NUM 4 // four columns
#define LED_PIN   22 // LED connected to GPIO22

char keys[ROW_NUM][COLUMN_NUM] = {
 {'1', '2', '3', 'A'},
 {'4', '5', '6', 'B'},
 {'7', '8', '9', 'C'},
 {'*', '0', '#', 'D'}
};

byte pin_rows[ROW_NUM]   = {13, 12, 14, 27}; // GPIO13, GPIO12, GPIO14, GPIO277 connect to the row pins
byte pin_column[COLUMN_NUM] = {26, 25, 33, 32};  // GPIO26, GPIO25, GPIO33, GPIO32 connect to the column pins

Keypad keypad = Keypad( makeKeymap(keys), pin_rows, pin_column, ROW_NUM, COLUMN_NUM );

// Access control variables
const String allowedCodes[] = {"1234", "5678", "9999"}; // List of allowed 4-digit codes
String enteredCode = ""; // Store the entered code
bool ledState = false; // Track LED state (off by default)

void setup() {
 Serial.begin(9600);
 pinMode(LED_PIN, OUTPUT);
 digitalWrite(LED_PIN, LOW); // Start with LED off

 // Set debounce time to reduce multiple key registrations (in milliseconds)
 keypad.setDebounceTime(50); // Adjust this value if needed (default is 10ms)

 Serial.println("Keypad Access Control System Ready");
 Serial.println("Enter 4-digit code and press # to toggle LED");
 Serial.println("Press * to clear entry");
}

void loop() {
 char key = keypad.getKey();

 if (key) {
  Serial.print("Key pressed: ");
  Serial.println(key);

  if (key == '#') {
   // Check the entered code
   if (enteredCode.length() == 4) {
    if (checkCode(enteredCode)) {
     // Valid code - toggle LED
     ledState = !ledState;
     digitalWrite(LED_PIN, ledState ? HIGH : LOW);
     Serial.print("Access GRANTED! LED is now ");
     Serial.println(ledState ? "ON" : "OFF");
    } else {
     Serial.println("Access DENIED! Invalid code.");
    }
   } else {
    Serial.println("Error: Please enter exactly 4 digits.");
   }
   enteredCode = ""; // Clear the entered code
  }
  else if (key == '*') {
   // Clear the entered code
   enteredCode = "";
   Serial.println("Entry cleared.");
  }
  else {
   // Add digit to entered code (limit to 4 digits)
   if (enteredCode.length() < 4) {
    enteredCode += key;
    Serial.print("Entered: ");
    for (int i = 0; i < enteredCode.length(); i++) {
     Serial.print("*"); // Display asterisks for security
    }
    Serial.println();
   } else {
    Serial.println("Max 4 digits reached. Press # to submit or * to clear.");
   }
  }
 }
}

// Function to check if entered code matches any allowed code
bool checkCode(String code) {
 int numCodes = sizeof(allowedCodes) / sizeof(allowedCodes[0]);
 for (int i = 0; i < numCodes; i++) {
  if (code == allowedCodes[i]) {
   return true;
  }
 }
 return false;
}
