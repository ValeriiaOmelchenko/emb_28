// Крок 2. volatile: без нього цикл не бачить зміну, зроблену в ISR.
//
// ISR кнопки BOOT ставить ДВА прапори: звичайний і volatile.
// loop() чекає на одному з них (USE_VOLATILE).
//   false → натискаєш BOOT, а програма «зависла»: компілятор прочитав plainFlag один раз
//           до циклу і крутить while(true) з копією в регістрі.
//   true  → кожна перевірка читає змінну з пам'яті, натискання видно.
// Перемкни USE_VOLATILE, перепроший і порівняй.

#include <Arduino.h>

constexpr uint8_t PIN_BOOT = 0;  // кнопка BOOT на DevKitC-1: замикає GPIO 0 на GND

constexpr bool USE_VOLATILE = false;

static bool plainFlag = false;
static volatile bool volatileFlag = false;

// IRAM_ATTR: код ISR лежить у RAM, бо під час запису у Flash кеш вимикається,
// а переривання може прийти саме тоді.
static void IRAM_ATTR onBootPressed() {
  plainFlag = true;
  volatileFlag = true;
}

void setup() {
  Serial.begin(115200);
  delay(500);

  // На платі вже є підтяжка 10 кОм до 3V3, внутрішня pull-up - про запас.
  pinMode(PIN_BOOT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_BOOT), onBootPressed, FALLING);

  Serial.printf("\n=== Крок 2: чекаю на %s прапор. Натискай BOOT ===\n",
                USE_VOLATILE ? "volatile" : "звичайний");
}

void loop() {
  // Навмисний busy-wait: у порожньому циклі немає викликів функцій,
  // тож компілятор має право не перечитувати звичайну змінну.
  if (USE_VOLATILE) {
    while (!volatileFlag) {
    }
    volatileFlag = false;
  } else {
    while (!plainFlag) {
    }
    plainFlag = false;
  }

  Serial.println("Побачила натискання BOOT");
}
