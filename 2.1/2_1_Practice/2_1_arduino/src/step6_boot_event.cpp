// Крок 6. Подія з ISR без втрат: std::atomic<bool> і exchange().
//
// Класичний шаблон з лекції:
//     if (flag) { обробка...; flag = false; }
// має «вікно»: якщо кнопку натиснули ПІД ЧАС обробки, ISR поставить flag = true,
// а рядок flag = false його зітре. Натискання загубилося.
//
// Правильно - забрати подію і скинути прапор однією неподільною операцією:
//     if (flag.exchange(false)) { обробка...; }
// Нове натискання під час обробки знову поставить прапор, і його обробить наступний прохід.
//
// USE_EXCHANGE перемикає два варіанти. Обробка навмисно повільна (HANDLING_MS),
// щоб вікно було великим. Натискай BOOT часто і порівнюй «ISR нарахувала» з «оброблено».

#include <Arduino.h>
#include <atomic>
#include "esp_timer.h"

constexpr uint8_t  PIN_BOOT    = 0;
constexpr uint32_t DEBOUNCE_US = 30000;
constexpr uint32_t HANDLING_MS = 400;  // імітація довгої обробки в суперциклі

constexpr bool USE_EXCHANGE = true;

static std::atomic<bool> bootEvent{false};
static std::atomic<uint32_t> isrPresses{0};
static uint32_t lastAcceptedUs = 0;  // лише в ISR

static void IRAM_ATTR onBootPressed() {
  const uint32_t nowUs = static_cast<uint32_t>(esp_timer_get_time());
  if (nowUs - lastAcceptedUs < DEBOUNCE_US) {
    return;
  }
  lastAcceptedUs = nowUs;

  isrPresses.fetch_add(1, std::memory_order_relaxed);
  bootEvent.store(true, std::memory_order_release);
}

static void handlePress(uint32_t handled) {
  Serial.printf("ISR нарахувала: %u  оброблено: %u\n",
                static_cast<unsigned>(isrPresses.load(std::memory_order_relaxed)),
                static_cast<unsigned>(handled));
  delay(HANDLING_MS);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(PIN_BOOT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_BOOT), onBootPressed, FALLING);

  Serial.printf("\n=== Крок 6: %s. atomic<bool> lock-free: %s ===\n",
                USE_EXCHANGE ? "exchange(false)" : "if (flag) {...; flag = false;}",
                bootEvent.is_lock_free() ? "так" : "ні");
}

void loop() {
  static uint32_t handled = 0;

  if (USE_EXCHANGE) {
    if (bootEvent.exchange(false, std::memory_order_acquire)) {
      ++handled;
      handlePress(handled);
    }
  } else {
    if (bootEvent.load(std::memory_order_acquire)) {
      ++handled;
      handlePress(handled);
      bootEvent.store(false);  // ← тут стирається натискання, що прийшло під час handlePress()
    }
  }
}
