// Крок 3. ISR рахує натискання BOOT: сирі фронти vs після debounce.
//
// Trust No Pin: одне натискання механічної кнопки - це кілька фронтів (брязкіт контактів).
// ISR рахує:
//   rawEdges        - кожен спадний фронт;
//   acceptedPresses - лише фронти, між якими пройшло ≥ DEBOUNCE_US.
// loop() раз на PRINT_PERIOD_MS читає лічильники і друкує, якщо щось змінилось.

#include <Arduino.h>
#include "esp_timer.h"

constexpr uint8_t  PIN_BOOT        = 0;
constexpr uint32_t DEBOUNCE_US     = 30000;  // 30 мс: довше за типовий брязкіт (1-10 мс)
constexpr uint32_t PRINT_PERIOD_MS = 50;

// Пише ISR, читає loop() → volatile.
// 32-бітне читання на 32-бітному ядрі - одна інструкція, тому «розірваним» воно не буде (див. крок 5).
static volatile uint32_t rawEdges = 0;
static volatile uint32_t acceptedPresses = 0;

// Потрібна лише всередині ISR → volatile не потрібен.
static uint32_t lastAcceptedUs = 0;

static void IRAM_ATTR onBootEdge() {
  // Запис «x = x + 1», а не «++x»: інкремент volatile застарілий з C++20 і
  // нагадує, що це НЕ атомарна операція (читання + додавання + запис).
  // Тут безпечно, бо пише лише одна ISR.
  rawEdges = rawEdges + 1;

  // esp_timer_get_time() дозволено викликати з ISR. Беззнакове віднімання переживає переповнення.
  const uint32_t nowUs = static_cast<uint32_t>(esp_timer_get_time());
  if (nowUs - lastAcceptedUs >= DEBOUNCE_US) {
    lastAcceptedUs = nowUs;
    acceptedPresses = acceptedPresses + 1;
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(PIN_BOOT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_BOOT), onBootEdge, FALLING);

  Serial.println("\n=== Крок 3: натискай BOOT, порівнюй фронти і натискання ===");
}

void loop() {
  static uint32_t lastPrintMs = 0;
  static uint32_t shownEdges = 0;

  if (millis() - lastPrintMs > PRINT_PERIOD_MS) {
  lastPrintMs = millis();
  const uint32_t edges = rawEdges;
  const uint32_t presses = acceptedPresses;

  if (edges != shownEdges) {
    shownEdges = edges;
    Serial.printf("фронтів: %u  натискань після debounce: %u\n",
                  static_cast<unsigned>(edges), static_cast<unsigned>(presses));
  }
  }

//комунікація по uart
  // Знімок: читаємо кожну volatile один раз і далі працюємо з локальними копіями.
  
}
