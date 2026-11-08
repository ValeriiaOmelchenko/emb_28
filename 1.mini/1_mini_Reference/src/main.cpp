// Еталон 1.mini - сутінковий вимикач на ESP32-S3.
//
// Кожні ~100 мс:
//   1. зчитати АЦП (середнє з 16 вимірів, 0...4095);
//   2. порівняти з двома порогами (гістерезис):
//        ADC < THRESHOLD_DARK  -> темно  -> HIGH на ключ (LED світить);
//        ADC > THRESHOLD_LIGHT -> світло -> LOW на ключ (LED згас);
//        між порогами          -> стан не змінюємо;
//   3. вивести adc / пороги / стан у форматі Serial Plotter.
//
// Схема (деталі й розрахунки в README.md):
//   3V3 --[ LDR ]--+--[ R2 10 кОм ]-- GND
//                  |
//                GPIO4 (ADC1_CH3)
//
//   GPIO5 --[ R3 10 кОм ]-- база BC547B, емітер -> GND
//   +5V --[ R4 270 Ом ]-- анод LED, катод LED -> колектор BC547B

#include <Arduino.h>

// ---------- Піни ----------
constexpr uint8_t PIN_LDR     = 4;  // ADC1: GPIO 1-10 на ESP32-S3, працює і з Wi-Fi
constexpr uint8_t PIN_LED_KEY = 5;  // через R3 на базу BC547B

// ---------- Таймінги ----------
constexpr uint32_t SERIAL_BAUD    = 115200;
constexpr uint32_t LOOP_PERIOD_MS = 100;

// ---------- АЦП ----------
constexpr uint8_t  ADC_BITS = 12;
constexpr uint16_t ADC_MAX  = (1u << ADC_BITS) - 1;  // 4095
constexpr uint8_t  SAMPLES  = 16;                    // степінь двійки: ділення стане зсувом

// ---------- Пороги ----------
// Підбираються за власними вимірюваннями (див. README, «Як обрати пороги»).
// Приклад: кімната ≈ 2900, долоня над LDR ≈ 700, шум після усереднення ≈ ±10.
// Мертва зона 500 кодів набагато ширша за шум, тому LED не мерехтить.
constexpr uint16_t THRESHOLD_DARK  = 1500;
constexpr uint16_t THRESHOLD_LIGHT = 2000;

static_assert(THRESHOLD_DARK < THRESHOLD_LIGHT, "THRESHOLD_DARK must be below THRESHOLD_LIGHT");
static_assert(THRESHOLD_LIGHT <= ADC_MAX, "THRESHOLD_LIGHT is out of ADC range");

// Поточний стан виходу. Саме він робить гістерезис можливим:
// між порогами рішення залежить від того, що було раніше.
static bool isLedOn = false;

// ---------- Вимірювання ----------

// Середнє з SAMPLES вимірів. Сума в uint32_t: 16 × 4095 = 65 520 - впритул до межі uint16_t.
static uint16_t readLdr() {
  uint32_t sum = 0;
  for (uint8_t i = 0; i < SAMPLES; ++i) {
    sum += analogRead(PIN_LDR);
  }
  return static_cast<uint16_t>((sum + SAMPLES / 2) / SAMPLES);  // + SAMPLES/2 - округлення
}

// ---------- Логіка ----------

// Гістерезис: нове рішення = f(вимір, поточний стан).
// Функція не чіпає піни, тому її легко перевірити подумки або тестом.
static bool decideLedState(uint16_t adc, bool ledOnNow) {
  if (adc < THRESHOLD_DARK) {
    return true;   // темно - вмикаємо
  }
  if (adc > THRESHOLD_LIGHT) {
    return false;  // світло - вимикаємо
  }
  return ledOnNow;  // мертва зона - нічого не змінюємо
}

static void setLed(bool on) {
  digitalWrite(PIN_LED_KEY, on ? HIGH : LOW);
}

// ---------- Вивід ----------

// Формат Serial Plotter: name:value. Пороги - горизонтальні лінії,
// led масштабовано до ADC_MAX, щоб перемикання було видно на тому ж графіку.
static void printState(uint16_t adc) {
  Serial.printf("adc:%u dark:%u light:%u led:%u\n",
                static_cast<unsigned>(adc),
                static_cast<unsigned>(THRESHOLD_DARK),
                static_cast<unsigned>(THRESHOLD_LIGHT),
                static_cast<unsigned>(isLedOn ? ADC_MAX : 0));
}

// ---------- Arduino ----------

void setup() {
  // Вихід - першим і одразу в LOW: навантаження в безпечному стані до першого вимірювання.
  pinMode(PIN_LED_KEY, OUTPUT);
  setLed(false);

  Serial.begin(SERIAL_BAUD);

  analogReadResolution(ADC_BITS);
  // 11 дБ - діапазон приблизно до 3,1 В. Це і так типове значення,
  // але явний рядок документує, з яким діапазоном ми працюємо.
  analogSetPinAttenuation(PIN_LDR, ADC_11db);
}

void loop() {
  const uint16_t adc = readLdr();

  const bool newState = decideLedState(adc, isLedOn);
  if (newState != isLedOn) {
    isLedOn = newState;
    setLed(isLedOn);
  }

  printState(adc);
  delay(LOOP_PERIOD_MS);  // у модулі 1 delay() дозволено
}
