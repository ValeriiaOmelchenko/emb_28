// Модуль 1.4 — Два світлодіоди, зовнішня кнопка та BOOT (еталонне рішення).
//
// Схема (ESP32-S3-DevKitC-1, 3.3 В):
//   GPIO15 ── 220 Ом ──▶|── GND    LED 1
//   GPIO16 ── 220 Ом ──▶|── GND    LED 2
//   GPIO21 ── кнопка ── GND        зовнішня кнопка, підтяжка — внутрішня (INPUT_PULLUP)
//   GPIO0  ── кнопка BOOT на платі (зовнішня підтяжка вже розпаяна)
//
// Логіка кнопок інверсна: відпущено = HIGH, натиснуто = LOW.
//
// Головна ідея: delay(1000) «глухий» — поки він іде, кнопки ніхто не читає.
// Тому чекаємо через waitMs(): той самий delay, але дрібними кроками
// по 1 мс, і на кожному кроці опитуємо кнопки.

#include <Arduino.h>

// ── Піни ─────────────────────────────────────────────────────────────
constexpr uint8_t PIN_LED_1    = 15;
constexpr uint8_t PIN_LED_2    = 16;
constexpr uint8_t PIN_BTN_EXT  = 21;  // режим 1 — синхронний
constexpr uint8_t PIN_BTN_BOOT = 0;   // режим 2 — по черзі (strapping-пін!)

// ── Таймінги ─────────────────────────────────────────────────────────
constexpr uint32_t SYNC_PERIOD_MS      = 200;
constexpr uint32_t ALTERNATE_PERIOD_MS = 1000;

// ── Режими ───────────────────────────────────────────────────────────
enum class Mode { Sync, Alternate };

Mode mode = Mode::Sync;  // режим за замовчуванням — №1

const char* modeName(Mode m) {
  return m == Mode::Sync ? "1: Sync (200 ms)" : "2: Alternate (1000 ms)";
}

bool isPressed(uint8_t pin) {
  return digitalRead(pin) == LOW;  // pull-up → натиснуто = LOW
}

// Кнопка не ПЕРЕМИКАЄ режим, а ВСТАНОВЛЮЄ конкретний.
// Тому брязкіт не шкодить: 10 «відскоків» 10 разів встановлять той самий режим.
void pollButtons() {
  Mode requested = mode;
  if (isPressed(PIN_BTN_EXT)) {
    requested = Mode::Sync;
  } else if (isPressed(PIN_BTN_BOOT)) {
    requested = Mode::Alternate;
  }

  if (requested != mode) {
    mode = requested;
    Serial.printf("Mode -> %s\n", modeName(mode));
  }
}

// Чекає `ms` мілісекунд, весь час слухаючи кнопки.
// Повертає false, якщо під час очікування змінився режим, —
// тоді поточний патерн треба перервати й почати новий одразу.
bool waitMs(uint32_t ms) {
  const Mode startMode = mode;
  for (uint32_t i = 0; i < ms; ++i) {
    pollButtons();
    if (mode != startMode) {
      return false;
    }
    delay(1);
  }
  return true;
}

void setLeds(bool led1, bool led2) {
  digitalWrite(PIN_LED_1, led1 ? HIGH : LOW);
  digitalWrite(PIN_LED_2, led2 ? HIGH : LOW);
}

void runSync() {
  setLeds(true, true);
  if (!waitMs(SYNC_PERIOD_MS)) return;
  setLeds(false, false);
  waitMs(SYNC_PERIOD_MS);
}

void runAlternate() {
  setLeds(true, false);
  if (!waitMs(ALTERNATE_PERIOD_MS)) return;
  setLeds(false, true);
  waitMs(ALTERNATE_PERIOD_MS);
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_LED_1, OUTPUT);
  pinMode(PIN_LED_2, OUTPUT);
  setLeds(false, false);

  pinMode(PIN_BTN_EXT, INPUT_PULLUP);  // внутрішня підтяжка
  pinMode(PIN_BTN_BOOT, INPUT);        // підтяжка вже є на платі

  Serial.printf("Start. Mode %s\n", modeName(mode));
}

void loop() {
  if (mode == Mode::Sync) {
    runSync();
  } else {
    runAlternate();
  }
}
