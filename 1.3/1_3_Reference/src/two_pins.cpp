// Модуль 1.3 — Поліцейська мигалка (еталонне рішення, базовий варіант).
//
// Схема (ESP32-S3, логіка 3.3 В):
//   GPIO4 ── 150 Ом ──▶|── GND   червоний LED (Vf ≈ 2.0 В → I ≈ 8.7 мА)
//   GPIO5 ──  47 Ом ──▶|── GND   синій LED    (Vf ≈ 2.9 В → I ≈ 8.5 мА)
//   GPIO6 ── 150 Ом ──▶|── GND   третій LED (додатково, білий/жовтий — перерахуй під свій Vf)
//
// Резистори пораховано для 3.3 В, а НЕ для 5 В: R = (3.3 - Vf) / I.
// Струм ~10 мА на пін — з запасом для GPIO і достатньо яскраво.

#include <Arduino.h>

// ── Піни ─────────────────────────────────────────────────────────────
// Не використовуємо strapping-піни (0, 3, 45, 46), USB (19, 20)
// і піни флешу/PSRAM (26–37) — див. Task.md.
constexpr uint8_t PIN_RED   = 4;
constexpr uint8_t PIN_BLUE  = 5;
constexpr uint8_t PIN_EXTRA = 6;

// ── Патерн ───────────────────────────────────────────────────────────
constexpr uint8_t  FLASHES_PER_SIDE = 3;    // «трійний» спалах, як у справжньої мигалки
constexpr uint32_t FLASH_ON_MS      = 70;
constexpr uint32_t FLASH_OFF_MS     = 50;
constexpr uint32_t SIDE_PAUSE_MS    = 120;  // пауза між червоною і синьою стороною

// ── Додатково: зміна швидкості ───────────────────────────────────────
// Кожні CYCLES_PER_SPEED циклів мигалка переходить на наступну швидкість.
constexpr uint8_t SPEED_PERCENT[]  = {100, 60, 150};  // % від базових таймінгів
constexpr uint8_t SPEED_COUNT      = sizeof(SPEED_PERCENT) / sizeof(SPEED_PERCENT[0]);
constexpr uint8_t CYCLES_PER_SPEED = 5;

uint8_t speedIndex = 0;

// Масштабує базовий час під поточну швидкість.
uint32_t scaled(uint32_t baseMs) {
  return baseMs * SPEED_PERCENT[speedIndex] / 100;
}

// Блимає світлодіодом на піні `pin` задану кількість разів.
void flash(uint8_t pin, uint8_t count) {
  for (uint8_t i = 0; i < count; ++i) {
    digitalWrite(pin, HIGH);
    delay(scaled(FLASH_ON_MS));
    digitalWrite(pin, LOW);
    delay(scaled(FLASH_OFF_MS));
  }
}

void setup() {
  Serial.begin(115200);

  // Без pinMode(OUTPUT) пін лишається входом: LED або не світить,
  // або ледь жевріє через внутрішню підтяжку.
  pinMode(PIN_RED, OUTPUT);
  pinMode(PIN_BLUE, OUTPUT);
  pinMode(PIN_EXTRA, OUTPUT);

  // Відомий стан на старті: все вимкнено.
  digitalWrite(PIN_RED, LOW);
  digitalWrite(PIN_BLUE, LOW);
  digitalWrite(PIN_EXTRA, LOW);

  Serial.println("Police flasher started");
}

void loop() {
  static uint8_t cycle = 0;

  flash(PIN_RED, FLASHES_PER_SIDE);
  delay(scaled(SIDE_PAUSE_MS));

  flash(PIN_BLUE, FLASHES_PER_SIDE);
  delay(scaled(SIDE_PAUSE_MS));

  // Додатково: третій LED — короткий «стробоскоп» наприкінці циклу.
  flash(PIN_EXTRA, 1);

  // Додатково: зміна швидкості.
  if (++cycle >= CYCLES_PER_SPEED) {
    cycle = 0;
    speedIndex = (speedIndex + 1) % SPEED_COUNT;
    Serial.printf("Speed: %u%%\n", SPEED_PERCENT[speedIndex]);
  }
}
