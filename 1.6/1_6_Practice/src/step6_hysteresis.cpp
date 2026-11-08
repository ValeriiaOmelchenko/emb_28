// Крок 6 (опційно). Гістерезис: два пороги замість одного.
//
// Навіть після згладжування число біля порогу може повільно «перетікати» туди-сюди.
// Рішення — як у термостата: вмикаємо при одному значенні, вимикаємо при іншому.
//
//   avg:   0 ─────── ON_BELOW ═══ «мертва зона» ═══ OFF_ABOVE ─────── 4095
//          ← темно: вмикаємо                     світло: вимикаємо →
//
// Поки число всередині «мертвої зони» — LED не змінює стан.
// Ширина зони має бути більшою за розмах шуму (дивіться крок 4).

#include <Arduino.h>

constexpr uint8_t  PIN_LDR        = 4;
constexpr uint8_t  PIN_LED        = 7;
constexpr uint32_t READ_PERIOD_MS = 20;

constexpr int ON_BELOW  = 1400;  // стало темніше за це — вмикаємо
constexpr int OFF_ABOVE = 1600;  // стало світліше за це — вимикаємо

constexpr uint8_t WINDOW_SHIFT = 3;
constexpr uint8_t WINDOW_SIZE  = 1 << WINDOW_SHIFT;

uint16_t readings[WINDOW_SIZE];
uint8_t  writeIndex = 0;
uint32_t sum        = 0;
bool     ledOn      = false;

uint16_t readFiltered() {
  uint16_t raw = analogRead(PIN_LDR);
  sum -= readings[writeIndex];
  readings[writeIndex] = raw;
  sum += raw;
  writeIndex = (writeIndex + 1) % WINDOW_SIZE;
  return sum >> WINDOW_SHIFT;
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  uint16_t first = analogRead(PIN_LDR);
  for (uint8_t i = 0; i < WINDOW_SIZE; ++i) {
    readings[i] = first;
  }
  sum = static_cast<uint32_t>(first) * WINDOW_SIZE;
}

void loop() {
  uint16_t avg = readFiltered();

  if (!ledOn && avg < ON_BELOW) {
    ledOn = true;
  } else if (ledOn && avg > OFF_ABOVE) {
    ledOn = false;
  }
  // Всередині зони [ON_BELOW; OFF_ABOVE] — нічого не робимо: стан «пам'ятається».

  digitalWrite(PIN_LED, ledOn ? HIGH : LOW);

  Serial.print("avg:");
  Serial.print(avg);
  Serial.print(" on_below:");
  Serial.print(ON_BELOW);
  Serial.print(" off_above:");
  Serial.print(OFF_ABOVE);
  Serial.print(" led:");
  Serial.println(ledOn ? 1000 : 0);

  delay(READ_PERIOD_MS);
}
