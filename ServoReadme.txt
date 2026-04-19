Ultrasonic Sensor (HC-SR04)

HC-SR04
VCC  → 5V
GND  → GND
TRIG → GPIO 4
ECHO → GPIO 5  ⚠️ (use voltage divider → 5V → 3.3V)

ECHO → GPIO 5 ⚠️ (Critical)
Sensor sends back a pulse
Pulse length = distance

BUT:

👉 ECHO outputs 5V
👉 ESP32 GPIO is ONLY 3.3V tolerant

Why voltage divider is required

If you connect directly:

ECHO → ESP32 GPIO

👉 You risk:

GPIO damage
Random resets
Permanent failure


Safe solution: Voltage Divider

Use 2 resistors to step down 5V → ~3.3V

🔧 Example values:
R1 = 1kΩ
R2 = 2kΩ
🔌 Wiring:
ECHO ---- R1 ----+---- GPIO 5 (ESP32)
                 |
                R2
                 |
                GND

Output voltage formula
Vout = Vin × (R2 / (R1 + R2))

With:

Vin = 5V
R1 = 1k
R2 = 2k

👉 Output ≈ 3.3V ✔️ safe

https://www.youtube.com/watch?v=U50lDRi3LXU

