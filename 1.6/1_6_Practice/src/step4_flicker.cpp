// Крок 4. Проблема: чому LED мерехтить на порозі?
//
// Той самий нічник, що й у кроці 3, але тепер рахуємо,
// скільки разів за секунду LED перемикається.
//
// Демонстрація: повільно накривайте LDR долонею так, щоб число
// «зависло» біля порогу. LED почне тремтіти, а лічильник — рости.
// Причина — шум: навіть при незмінному світлі adc «скаче» на кілька одиниць
// і раз за разом перетинає поріг туди-сюди.
//
// Як це виправити — кроки 5 (згладжування) і 6 (гістерезис).

#include <Arduino.h>

constexpr uint8_t  PIN_LDR        = 4;
constexpr uint8_t  PIN_LED        = 7;
constexpr uint32_t READ_PERIOD_MS = 10;   // читаємо часто, щоб побачити кожне «тремтіння»
constexpr int      THRESHOLD      = 1500; // той самий поріг, що в кроці 3

constexpr uint16_t READS_PER_REPORT = 100;  // 100 × 10 мс ≈ 1 секунда

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
  Serial.println("Step 4: LED toggles per second");
}

void loop() {
  static bool     ledOn    = false;
  static uint16_t toggles  = 0;
  static uint16_t reads    = 0;
  static int      minAdc   = 4095;
  static int      maxAdc   = 0;

  int adc = analogRead(PIN_LDR);
  minAdc = min(minAdc, adc);
  maxAdc = max(maxAdc, adc);

  bool isDark = adc < THRESHOLD;
  if (isDark != ledOn) {
    ledOn = isDark;
    digitalWrite(PIN_LED, ledOn ? HIGH : LOW);
    toggles++;
  }

  if (++reads >= READS_PER_REPORT) {
    // Розмах шуму (max - min) показує, наскільки «тремтить» число за секунду.
    Serial.printf("adc: %4d..%4d  (noise %3d)   LED toggles/s: %u\n",
                  minAdc, maxAdc, maxAdc - minAdc, toggles);
    reads   = 0;
    toggles = 0;
    minAdc  = 4095;
    maxAdc  = 0;
  }

  delay(READ_PERIOD_MS);
}
