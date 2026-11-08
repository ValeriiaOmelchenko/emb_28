// Еталон 2.3 - три незалежні світлодіоди в одному superloop без delay().
// ESP32-S3-DevKitC-1, Arduino.
//
//   червоний - перемикається кожні 200 мс;
//   зелений  - кожні 500 мс;
//   синій    - кожні 1000 мс.
// Раз на 10 с у Serial іде контрольний звіт: скільки перемикань зробив кожен LED
// (очікуємо 50 / 20 / 10) і найдовший прохід loop().

#include <Arduino.h>
#include <inttypes.h>

#include "blinker.h"
#include "config.h"

// Таблиця задач: статичний масив фіксованого розміру. Четвертий LED = один рядок.
static Blinker blinkers[] = {
    {"red",   config::LED_RED_PIN,   config::RED_TOGGLE_MS,   0, 0, false},
    {"green", config::LED_GREEN_PIN, config::GREEN_TOGGLE_MS, 0, 0, false},
    {"blue",  config::LED_BLUE_PIN,  config::BLUE_TOGGLE_MS,  0, 0, false},
};

// Стан звіту і профілю циклу.
struct Report {
  uint32_t lastMs;      // початок поточного вікна звіту
  uint32_t prevLoopUs;  // початок попереднього проходу loop()
  uint32_t maxLoopUs;   // найдовший прохід за вікно = найгірший час реакції
};

static Report report = {0, 0, 0};

static void reportInit(Report& r, uint32_t nowMs) {
  r.lastMs     = nowMs;
  r.prevLoopUs = micros();
  r.maxLoopUs  = 0;
}

static void reportUpdate(Report& r, uint32_t nowMs) {
  // Різниця між початками сусідніх проходів: сюди потрапляє весь прохід,
  // включно з накладними витратами Arduino між викликами loop().
  const uint32_t nowUs  = micros();
  const uint32_t loopUs = nowUs - r.prevLoopUs;
  r.prevLoopUs = nowUs;
  if (loopUs > r.maxLoopUs) {
    r.maxLoopUs = loopUs;
  }

  if (nowMs - r.lastMs < config::REPORT_PERIOD_MS) {
    return;
  }
  r.lastMs += config::REPORT_PERIOD_MS;

  Serial.printf("[%7" PRIu32 " ms] toggles:", nowMs);
  for (Blinker& b : blinkers) {
    Serial.printf(" %s=%" PRIu32, b.name, b.toggles);
    b.toggles = 0;
  }
  Serial.printf("  max loop=%" PRIu32 " us\n", r.maxLoopUs);

  r.maxLoopUs  = 0;
  r.prevLoopUs = micros();  // час самого друку в наступне вікно не потрапляє
}

void setup() {
  Serial.begin(config::SERIAL_BAUD);

  // Одна точка відліку для всіх: сітки часу трьох LED збігаються,
  // і раз на секунду всі три перемикаються в одному проході.
  const uint32_t startMs = millis();
  for (Blinker& b : blinkers) {
    blinkerInit(b, startMs);
  }
  reportInit(report, startMs);

  Serial.printf("Etalon 2.3: red %" PRIu32 " ms, green %" PRIu32 " ms, blue %" PRIu32 " ms\n",
                config::RED_TOGGLE_MS, config::GREEN_TOGGLE_MS, config::BLUE_TOGGLE_MS);
}

void loop() {
  // Один знімок часу на прохід: усі задачі бачать однаковий «зараз».
  const uint32_t nowMs = millis();

  for (Blinker& b : blinkers) {
    blinkerUpdate(b, nowMs);
  }
  reportUpdate(report, nowMs);
}
