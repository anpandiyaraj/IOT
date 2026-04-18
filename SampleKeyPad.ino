#include <Keypad.h>
#include <string.h>

// ---------------- PIN CONFIG ----------------
#define ROW_NUM     4
#define COLUMN_NUM  4
#define LED_PIN     22

// ---------------- CODE CONFIG ----------------
#define CODE_LENGTH 8   // change this to desired length

// ---------------- KEYPAD ----------------
char keys[ROW_NUM][COLUMN_NUM] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

byte pin_rows[ROW_NUM]    = {13, 12, 14, 27};
byte pin_column[COLUMN_NUM] = {26, 25, 33, 32};

Keypad keypad = Keypad(makeKeymap(keys), pin_rows, pin_column, ROW_NUM, COLUMN_NUM);

// ---------------- ACCESS CONTROL ----------------
const char* allowedCodes[] = {"72953689"};   // must match CODE_LENGTH
const int numCodes = sizeof(allowedCodes) / sizeof(allowedCodes[0]);

char enteredCode[CODE_LENGTH + 1];  // buffer for digits + null
byte codeIndex = 0;

bool ledState = false;

// ---------------- HELPERS ----------------
bool isDigitKey(char key) {
  return (key >= '0' && key <= '9');
}

bool checkCode(const char* code) {
  for (int i = 0; i < numCodes; i++) {
    if (strcmp(code, allowedCodes[i]) == 0) {
      return true;
    }
  }
  return false;
}

void resetInput() {
  codeIndex = 0;
  enteredCode[0] = '\0';
}

// ---------------- SETUP ----------------
void setup() {
  Serial.begin(115200);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  keypad.setDebounceTime(50);

  resetInput();

  Serial.println("=== Keypad Access Control ===");
  Serial.print("Enter ");
  Serial.print(CODE_LENGTH);
  Serial.println("-digit code + #");
  Serial.println("* = Clear");
}

// ---------------- LOOP ----------------
void loop() {
  char key = keypad.getKey();
  if (!key) return;

  Serial.print("Key: ");
  Serial.println(key);

  // ---------------- CLEAR ----------------
  if (key == '*') {
    resetInput();
    Serial.println("Cleared");
    return;
  }

  // ---------------- SUBMIT ----------------
  if (key == '#') {
    if (codeIndex == CODE_LENGTH) {
      if (checkCode(enteredCode)) {
        ledState = !ledState;
        digitalWrite(LED_PIN, ledState);

        Serial.print("ACCESS GRANTED. LED: ");
        Serial.println(ledState ? "ON" : "OFF");
      } else {
        Serial.println("ACCESS DENIED");
      }
    } else {
      Serial.print("Enter exactly ");
      Serial.print(CODE_LENGTH);
      Serial.println(" digits");
    }
    resetInput();
    return;
  }

  // ---------------- IGNORE NON-DIGITS ----------------
  if (!isDigitKey(key)) {
    Serial.println("Invalid key (only 0-9 allowed)");
    return;
  }

  // ---------------- ADD DIGIT ----------------
  if (codeIndex < CODE_LENGTH) {
    enteredCode[codeIndex++] = key;
    enteredCode[codeIndex] = '\0';

    Serial.print("Entered: ");
    for (int i = 0; i < codeIndex; i++) Serial.print("*");
    Serial.println();
  } else {
    // Strict block after CODE_LENGTH digits
    Serial.print("Only ");
    Serial.print(CODE_LENGTH);
    Serial.println(" digits allowed. Press # or *");
  }
}
