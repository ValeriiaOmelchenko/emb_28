// Еталон 1.6 — LDR у подільнику напруги, АЦП ESP32-S3.
//
// Кожні ~100 мс:
//   1. RAW з analogRead();
//   2. напруга за формулою  U = RAW / ADC_MAX * U_REF;
//   3. напруга з analogReadMilliVolts() (заводська калібровка з eFuse);
//   4. абсолютна (мВ) і відносна (%) похибка формули відносно каліброваного значення.
//
// Схема:
//   3V3 ──[ LDR ]──┬──[ 10 кОм ]── GND
//                  │
//                GPIO4  (ESP32-S3: ADC1_CH3 — працює і з увімкненим Wi-Fi)

#include <Arduino.h>
#include <limits>

// ---------- Конфігурація ----------
constexpr uint8_t  PIN_LDR        = 4;      // ADC1: GPIO 1–10 на ESP32-S3
constexpr uint32_t SERIAL_BAUD    = 115200;
constexpr uint32_t READ_PERIOD_MS = 100;

constexpr uint8_t  ADC_BITS  = 12;
constexpr uint16_t ADC_MAX   = (1u << ADC_BITS) - 1;  // 4095
constexpr uint16_t U_REF_MV  = 3100;                  // з умови завдання

// Кожен рядок виводу — середнє з SAMPLES пар вимірів.
// Степінь двійки → компілятор замінить ділення зсувом.
constexpr uint8_t  SAMPLES   = 16;

// Тип акумулятора в measure(). Сума 16 × 4095 = 65 520 — впритул до межі uint16_t,
// тому uint32_t. Мілівольти (≤ ~3400) менші за ADC_MAX, тож перевірка RAW покриває й їх.
using AdcSum = uint32_t;

// Перевірки під час компіляції: порушене припущення = прошивка не збереться.
static_assert(static_cast<uint64_t>(SAMPLES) * ADC_MAX <= std::numeric_limits<AdcSum>::max(),
              "AdcSum is too small for SAMPLES * ADC_MAX");
static_assert(SAMPLES != 0 && (SAMPLES & (SAMPLES - 1)) == 0,
              "SAMPLES must be a power of two");

struct Measurement {
  uint16_t raw;         // середній RAW
  uint16_t rawSpan;     // max − min за серію: наочна міра шуму
  uint16_t measuredMv;  // середнє analogReadMilliVolts()
};

// ---------- Вимірювання ----------

// analogRead() і analogReadMilliVolts() — це ДВА окремі перетворення АЦП.
// Читаємо їх почергово в одному циклі, щоб обидва середні
// відносилися до одного і того самого проміжку часу.
static Measurement measure() {
  AdcSum sumRaw = 0;
  AdcSum sumMv  = 0;
  uint16_t rawMin = ADC_MAX;
  uint16_t rawMax = 0;

  for (uint8_t i = 0; i < SAMPLES; ++i) {
    const uint16_t raw = analogRead(PIN_LDR);
    sumRaw += raw;
    sumMv  += analogReadMilliVolts(PIN_LDR);
    if (raw < rawMin) rawMin = raw;
    if (raw > rawMax) rawMax = raw;
  }

  return {
    static_cast<uint16_t>((sumRaw + SAMPLES / 2) / SAMPLES),  // + SAMPLES/2 — округлення, а не відкидання
    static_cast<uint16_t>(rawMax - rawMin),
    static_cast<uint16_t>((sumMv + SAMPLES / 2) / SAMPLES),
  };
}

// U = RAW / ADC_MAX * U_REF — у цілих мілівольтах.
// Множимо ДО ділення (інакше RAW / 4095 = 0) і в uint32_t (4095 × 3100 не влазить в uint16_t).
static uint16_t calcMillivolts(uint16_t raw) {
  return static_cast<uint16_t>(
      (static_cast<uint32_t>(raw) * U_REF_MV + ADC_MAX / 2) / ADC_MAX);
}

// ---------- Вивід ----------

static void printRow(const Measurement& m, uint16_t calcMv) {
  // Знак Δ показує напрям: «−» — формула занижує.
  const int32_t deltaMv = static_cast<int32_t>(calcMv) - m.measuredMv;

  Serial.printf("RAW=%u noise=%u calc=%u mV measured=%u mV delta=%ld mV error=",
                static_cast<unsigned>(m.raw),
                static_cast<unsigned>(m.rawSpan),
                static_cast<unsigned>(calcMv),
                static_cast<unsigned>(m.measuredMv),
                static_cast<long>(deltaMv));

  // Еталон — калібрована напруга, тож ділимо на неї.
  // У повній темряві вона може бути 0 — тоді похибка не визначена.
  if (m.measuredMv == 0) {
    Serial.println("---");
    return;
  }
  const float errorPct = 100.0f * static_cast<float>(deltaMv) / m.measuredMv;
  Serial.printf("%.2f %%\n", errorPct);
}

// ---------- Arduino ----------

void setup() {
  Serial.begin(SERIAL_BAUD);

  analogReadResolution(ADC_BITS);
  // 11 дБ — діапазон до ~3,1 В. Типове значення і так 11 дБ,
  // але явний рядок документує, з яким діапазоном ми працюємо.
  analogSetPinAttenuation(PIN_LDR, ADC_11db);
}

void loop() {
  const Measurement m = measure();
  printRow(m, calcMillivolts(m.raw));
  delay(READ_PERIOD_MS);
}
