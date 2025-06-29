#include <Keypad.h>

// Define keypad layout
const byte ROWS = 4;
const byte COLS = 4;

char keys[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

byte rowPins[ROWS] = {5, 4, 14, 12};   // Adjust these GPIOs as per wiring
byte colPins[COLS] = {13, 0, 16, 2};   // Adjust these GPIOs as per wiring

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// Hardcoded password
String password = "B1AC0A5";
String input = "";

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("-------------\nEnter the password: ");
}

void loop() {
  char key = keypad.getKey();

  if (key) {
    if (key == '*') {
      Serial.println();
      if (input == password) {
        Serial.println("-------------\nCorrect password!");
      } else {
        Serial.println("-------------\nIncorrect password!");
      }
      delay(3000);
      input = "";
      Serial.print("-------------\nEnter the password: ");
    } else if (key == 'D') {
      // Backspace logic
      if (input.length() > 0) {
        input.remove(input.length() - 1);
        Serial.println("\n<Backspace>");
        Serial.print(input);
      }
    } else {
      input += key;
      Serial.print(key);
    }
  }
}
