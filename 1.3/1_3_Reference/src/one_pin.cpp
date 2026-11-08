// Модуль 1.3 — Бонус: червоний і синій LED по черзі на ОДНОМУ GPIO.
//
// Схема (ESP32-S3, 3.3 В):
//
//          3V3
//           │
//          ─┴─  синій LED (анод до 3V3)
//          ▼
//           │
//          47 Ом
//           │
//   GPIO4 ──┤
//           │
//          150 Ом
//           │
//          ─┴─  червоний LED (анод до GPIO)
//          ▼
//           │
//          GND
//
//   GPIO = HIGH → струм тече GPIO → червоний → GND        (світить червоний)
//   GPIO = LOW  → струм тече 3V3 → синій → GPIO            (світить синій)
//   GPIO = INPUT (Hi-Z) → обидва вимкнені, АЛЕ лише тому, що
//          Vf(червоний) + Vf(синій) ≈ 2.0 + 2.9 = 4.9 В > 3.3 В.
//          З двома червоними LED (4.0 В) запас уже малий, а на 5-вольтовій
//          платі (Arduino UNO) обидва LED жевріли б у «вимкненому» стані.
//
// Для мигалки «по черзі» досить HIGH/LOW. Hi-Z потрібен лише для паузи.

#include <Arduino.h>

constexpr uint8_t PIN_FLASHER = 4;

constexpr uint8_t  FLASHES_PER_SIDE = 3;
constexpr uint32_t FLASH_ON_MS      = 70;
constexpr uint32_t FLASH_OFF_MS     = 50;
constexpr uint32_t SIDE_PAUSE_MS    = 120;

enum class Light { Red, Blue, Off };

// Один пін — три стани.
void show(Light light) {
  switch (light) {
    case Light::Red:
      pinMode(PIN_FLASHER, OUTPUT);
      digitalWrite(PIN_FLASHER, HIGH);
      break;
    case Light::Blue:
      pinMode(PIN_FLASHER, OUTPUT);
      digitalWrite(PIN_FLASHER, LOW);
      break;
    case Light::Off:
      pinMode(PIN_FLASHER, INPUT);  // Hi-Z: без підтяжок, пін «відпущено»
      break;
  }
}

void flash(Light light, uint8_t count) {
  for (uint8_t i = 0; i < count; ++i) {
    show(light);
    delay(FLASH_ON_MS);
    show(Light::Off);
    delay(FLASH_OFF_MS);
  }
}

void setup() {
  show(Light::Off);
}

void loop() {
  flash(Light::Red, FLASHES_PER_SIDE);
  delay(SIDE_PAUSE_MS);
  flash(Light::Blue, FLASHES_PER_SIDE);
  delay(SIDE_PAUSE_MS);
}
