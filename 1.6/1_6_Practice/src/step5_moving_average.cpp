// Крок 5. Фільтр ковзного середнього (moving average).
//
// Ідея: зберігаємо останні N вимірів у «кільцевому буфері».
// Кожен новий вимір: відняли з суми найстаріший, додали найновіший.
// Середнє = сума / N.
//
//   buffer: [ 812 | 805 | 820 | 799 ]   ← нове значення пише поверх найстарішого
//                       ^ writeIndex
//
// N — степінь двійки (4, 8, 16, 32): тоді ділення «/ N» можна замінити
// швидким бітовим зсувом «>> SHIFT». Для МК без апаратного ділення це суттєво.
//
// Експеримент: змініть WINDOW_SHIFT з 2 (вікно 4) на 5 (вікно 32) і порівняйте
// в Serial Plotter: AVG стає гладшим, але й «запізнюється» за RAW.

#include <Arduino.h>

constexpr uint8_t  PIN_LDR        = 4;
constexpr uint8_t  PIN_LED        = 7;
constexpr uint32_t READ_PERIOD_MS = 20;
constexpr int      THRESHOLD      = 1500;

constexpr uint8_t  WINDOW_SHIFT = 3;                  // 2 → 4, 3 → 8, 4 → 16, 5 → 32
constexpr uint8_t  WINDOW_SIZE  = 1 << WINDOW_SHIFT;  // завжди степінь двійки

uint16_t readings[WINDOW_SIZE];
uint8_t  writeIndex = 0;
uint32_t sum        = 0;  // 32 × 4095 = 131 040 — у uint16_t вже не влізе!

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);

  // Заповнюємо буфер першим виміром. Якщо почати з нулів,
  // перші N вимірів середнє буде «повзти» знизу і нічник увімкнеться на старті.
  uint16_t first = analogRead(PIN_LDR);
  for (uint8_t i = 0; i < WINDOW_SIZE; ++i) {
    readings[i] = first;
  }
  sum = static_cast<uint32_t>(first) * WINDOW_SIZE;
}

void loop() {
  uint16_t raw = analogRead(PIN_LDR);

  sum -= readings[writeIndex];   // відняли найстаріше
  readings[writeIndex] = raw;    // записали найновіше на його місце
  sum += raw;                    // додали найновіше
  writeIndex = (writeIndex + 1) % WINDOW_SIZE;  // по колу: 0, 1, …, N-1, 0, 1, …

  uint16_t avg = sum >> WINDOW_SHIFT;  // те саме, що sum / WINDOW_SIZE

  digitalWrite(PIN_LED, avg < THRESHOLD ? HIGH : LOW);

  Serial.print("raw:");
  Serial.print(raw);
  Serial.print(" avg:");
  Serial.println(avg);

  delay(READ_PERIOD_MS);
}
