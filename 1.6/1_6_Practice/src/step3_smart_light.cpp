// Крок 3. «Розумне світло»: темно → LED вмикається.
//
// Схема: подільник LDR на GPIO4 + світлодіод:
//   GPIO7 ── 220 Ом ──▶|── GND
//
// Поріг підберіть зі своїх чисел із кроку 1:
//   THRESHOLD ≈ посередині між «темрява» і «світло».
//
// Навмисна помилка для демонстрації: поміняйте `<` на `>` —
// світло почне вмикатися вдень. Хто знайде баг першим?

#include <Arduino.h>

constexpr uint8_t  PIN_LDR        = 4;
constexpr uint8_t  PIN_LED        = 7;
constexpr uint32_t READ_PERIOD_MS = 100;

constexpr int THRESHOLD = 1500;  // ← підберіть під своє освітлення

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
}

void loop() {
  int adc = analogRead(PIN_LDR);

  // Більше світла = більше число. Отже, мало світла = число нижче порогу.
  bool isDark = adc < THRESHOLD;
  digitalWrite(PIN_LED, isDark ? HIGH : LOW);

  Serial.print("adc:");
  Serial.print(adc);
  Serial.print(" threshold:");
  Serial.print(THRESHOLD);
  Serial.print(" led:");
  Serial.println(isDark ? 1000 : 0);  // 0 / 1000 — щоб «сходинку» LED було видно на одному графіку з adc

  delay(READ_PERIOD_MS);
}
