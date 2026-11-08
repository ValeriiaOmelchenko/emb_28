// Практика 2.3 - superloop: як змусити MCU робити кілька справ одночасно.
// ESP32-S3-DevKitC-1, Arduino.
//
// У superloop крутяться чотири незалежні «задачі»:
//   1) червоний LED блимає з півперіодом 500 мс;
//   2) синій LED блимає з півперіодом 100 мс;
//   3) кнопка (polling + debounce на millis()): кожне натискання перемикає зелений LED;
//   4) звіт у Serial раз на 5 с: скільки проходів loop(), найдовший прохід,
//      скільки перемикань синього і скільки натискань.
// Жодна задача нічого не чекає: перевірила «чи настав час?» і одразу повернулась.
//
// Перемикачі для демонстрації - константи в блоці «ДЕМО» нижче.

#include <Arduino.h>

// ============================ ЗАЛІЗО ============================
// GPIO 4-7: не strapping (0, 3, 45, 46), не USB (19, 20), не флеш/PSRAM (26-37).
constexpr uint8_t PIN_LED_RED   = 4;
constexpr uint8_t PIN_LED_GREEN = 5;
constexpr uint8_t PIN_LED_BLUE  = 6;
constexpr uint8_t PIN_BUTTON    = 7;   // кнопка між GPIO 7 і GND, INPUT_PULLUP → натиснута = LOW

// ============================ ДЕМО ============================
// Блокуюча пауза в кожному проході loop(): імітація «поганого» коду з delay()
// або бібліотеки, яка всередині чекає відповідь датчика.
//   0    → нормальний superloop: усе реагує миттєво;
//   7    → цикл ~7 мс: синій перемикається рідше, ніж треба (дрейф), див. USE_FIXED_RATE;
//   1000 → «delay(1000) у loop()»: обидва LED блимають раз на секунду, кнопка «глухне»,
//          коротке натискання губиться.
constexpr uint32_t BLOCKING_DELAY_MS = 0;

// Як переставляти таймер після спрацювання:
//   false → last = now          (відлік від фактичного моменту, середній період «пливе»);
//   true  → last += period      (тримаємо «сітку» часу, середній період точний).
constexpr bool USE_FIXED_RATE = false;

// ============================ ТАЙМІНГИ ============================
constexpr uint32_t RED_HALF_PERIOD_MS  = 500;
constexpr uint32_t BLUE_HALF_PERIOD_MS = 100;
constexpr uint32_t REPORT_PERIOD_MS    = 5000;
// Debounce = 2-3 × МАКСИМАЛЬНОГО брязкоту (урок 1.5: у більшості кнопок до ~6-10 мс).
constexpr uint32_t DEBOUNCE_MS         = 30;
constexpr uint32_t SERIAL_BAUD         = 115200;

// ============================ ТАЙМЕРИ LED ============================
struct Blinker {
  uint8_t  pin;
  uint32_t halfPeriodMs;
  uint32_t lastToggleMs;
  uint32_t toggles;      // лічильник для звіту
  bool     isOn;
};

// Статичний масив фіксованого розміру. Новий таймер = один рядок.
static Blinker blinkers[] = {
  {PIN_LED_RED,  RED_HALF_PERIOD_MS,  0, 0, false},
  {PIN_LED_BLUE, BLUE_HALF_PERIOD_MS, 0, 0, false},
};
constexpr size_t BLUE_INDEX = 1;  // чий лічильник перемикань друкуємо у звіті

static void updateBlinker(Blinker& b, uint32_t nowMs) {
  // Беззнакове віднімання: коректне і через переповнення millis() (~49,7 доби).
  if (nowMs - b.lastToggleMs < b.halfPeriodMs) {
    return;  // ще не час - одразу повертаємо керування
  }

  if (USE_FIXED_RATE) {
    b.lastToggleMs += b.halfPeriodMs;
    if (nowMs - b.lastToggleMs >= b.halfPeriodMs) {
      b.lastToggleMs = nowMs;  // відстали більше ніж на період - не наздоганяємо пачкою
    }
  } else {
    b.lastToggleMs = nowMs;
  }

  b.isOn = !b.isOn;
  digitalWrite(b.pin, b.isOn ? HIGH : LOW);
  b.toggles++;
}

// ============================ КНОПКА ============================
// Polling: на кожному проході дивимось на пін «зараз». Ловимо ПОДІЮ (натиснули),
// а не стан (натиснута). Новий рівень приймаємо, лише коли він протримався DEBOUNCE_MS.
struct Button {
  uint8_t  pin;
  bool     stableLevel;   // прийнятий рівень
  bool     lastRawLevel;  // сирий рівень на минулому проході
  uint32_t lastChangeMs;  // коли сирий рівень востаннє змінився
  uint32_t presses;       // скільки натискань прийнято
};

static Button button = {PIN_BUTTON, HIGH, HIGH, 0, 0};

// Повертає true рівно один раз на кожне натискання.
static bool updateButton(Button& btn, uint32_t nowMs) {
  const bool raw = digitalRead(btn.pin);

  // Кожна зміна сирого рівня перезапускає відлік. DEBOUNCE_MS - це вікно ТИШІ від
  // останнього стрибка, а не від першого. Фільтрує і натискання, і відпускання.
  if (raw != btn.lastRawLevel) {
    btn.lastRawLevel = raw;
    btn.lastChangeMs = nowMs;
  }

  if (raw == btn.stableLevel || nowMs - btn.lastChangeMs < DEBOUNCE_MS) {
    return false;
  }
  btn.stableLevel = raw;
  if (btn.stableLevel == LOW) {
    btn.presses++;
    return true;  // подія лише на натискання
  }
  return false;
}

static void updateGreen(bool pressEvent) {
  static bool isOn = false;
  if (pressEvent) {
    isOn = !isOn;
    digitalWrite(PIN_LED_GREEN, isOn ? HIGH : LOW);
  }
}

// ============================ «ПОГАНА» ЗАДАЧА ============================
static void blockingTask() {
  if (BLOCKING_DELAY_MS > 0) {
    delay(BLOCKING_DELAY_MS);  // навмисно: так виглядає блокуючий код
  }
}

// ============================ ЗВІТ ============================
// Тут же профіль superloop: скільки проходів і найдовший прохід.
// Найдовший прохід = найгірший час реакції на кнопку.
static void updateReport(uint32_t nowMs) {
  static uint32_t prevLoopUs   = micros();
  static uint32_t maxLoopUs    = 0;
  static uint32_t loops        = 0;
  static uint32_t lastReportMs = 0;

  const uint32_t nowUs = micros();
  const uint32_t loopUs = nowUs - prevLoopUs;
  prevLoopUs = nowUs;
  if (loopUs > maxLoopUs) {
    maxLoopUs = loopUs;
  }
  loops++;

  if (nowMs - lastReportMs < REPORT_PERIOD_MS) {
    return;
  }
  lastReportMs = nowMs;

  Blinker& blue = blinkers[BLUE_INDEX];
  Serial.printf("за %u с: проходів %u, найдовший %u мкс | синій: %u перемикань (очікується %u) | "
                "натискань %u\n",
                static_cast<unsigned>(REPORT_PERIOD_MS / 1000),
                static_cast<unsigned>(loops),
                static_cast<unsigned>(maxLoopUs),
                static_cast<unsigned>(blue.toggles),
                static_cast<unsigned>(REPORT_PERIOD_MS / BLUE_HALF_PERIOD_MS),
                static_cast<unsigned>(button.presses));

  loops = 0;
  maxLoopUs = 0;
  blue.toggles = 0;
  prevLoopUs = micros();  // час самого Serial.printf у наступний звіт не рахуємо
}

// ============================ SUPERLOOP ============================
void setup() {
  Serial.begin(SERIAL_BAUD);

  for (const Blinker& b : blinkers) {
    pinMode(b.pin, OUTPUT);
    digitalWrite(b.pin, LOW);       // відомий стан на старті (збігається з isOn = false)
  }
  pinMode(PIN_LED_GREEN, OUTPUT);
  digitalWrite(PIN_LED_GREEN, LOW);
  pinMode(PIN_BUTTON, INPUT_PULLUP);  // без підтяжки пін «плаває» (Trust No Pin)

  Serial.printf("\n=== Практика 2.3: блокуюча пауза %u мс, %s ===\n",
                static_cast<unsigned>(BLOCKING_DELAY_MS),
                USE_FIXED_RATE ? "last += period" : "last = now");
}

// Контракт superloop: кожна функція швидка, нічого не чекає і повертає керування.
void loop() {
  const uint32_t nowMs = millis();  // один знімок часу на весь прохід

  for (Blinker& b : blinkers) {
    updateBlinker(b, nowMs);
  }
  updateGreen(updateButton(button, nowMs));
  blockingTask();
  updateReport(nowMs);
}
