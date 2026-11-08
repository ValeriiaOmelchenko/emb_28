// Крок 1. Перше знайомство з АЦП: що «бачить» плата?
//
// Схема (подільник напруги):
//   3V3 ──[ LDR ]──┬──[ 10 кОм ]── GND
//                  │
//                GPIO4  (на ESP32-S3 це ADC1, канал 3)
//
// Що робимо: закриваємо LDR долонею, світимо ліхтариком — і дивимось, як змінюється число.
// Записати собі два числа: «темрява» і «світло» — вони знадобляться в кроці 3.

#include <Arduino.h>

constexpr uint8_t  PIN_LDR        = 4;
constexpr uint32_t READ_PERIOD_MS = 200;

void setup() {
  Serial.begin(115200);
  delay(500);  // даємо монітору порту підключитися
  Serial.println("Step 1: raw ADC value (0..4095)");
}

void loop() {
  int adc = analogRead(PIN_LDR);  // 12 біт: число від 0 до 4095

  Serial.print("ADC = ");
  Serial.println(adc);

  delay(READ_PERIOD_MS);
}
