// Крок 2. Від числа — до вольтів. Serial Plotter.
//
// Ідеальна модель АЦП: 0 В → 0, 3.3 В → 4095.
//   V = adc / 4095 × 3.3
// Приклад: adc = 2048 → ≈ 1.65 В — рівно половина шкали.
//
// Відкрийте Serial Plotter (Arduino IDE) або Teleplot (VS Code):
// рядки виду "adc:1234 volts:0.99" плоттер малює як два графіки.
//
// Питання до групи: якщо LDR = 10 кОм (як і нижній резистор),
// яку напругу і яке число ми побачимо?

#include <Arduino.h>

constexpr uint8_t  PIN_LDR        = 4;
constexpr uint32_t READ_PERIOD_MS = 50;  // частіше — щоб графік був плавним

constexpr float    VCC_V   = 3.3f;
constexpr int      ADC_MAX = 4095;  // 2^12 - 1: «сходинок» 4096, а найбільше число — 4095

void setup() {
  Serial.begin(115200);
}

void loop() {
  int adc = analogRead(PIN_LDR);

  // 4095.0f, а не 4095: інакше ділення буде цілочисельним і майже завжди дасть 0.
  float volts = adc / static_cast<float>(ADC_MAX) * VCC_V;

  Serial.print("adc:");
  Serial.print(adc);
  Serial.print(" volts:");
  Serial.println(volts, 2);

  delay(READ_PERIOD_MS);
}
