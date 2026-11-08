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

#if PRACTICE_STEP == 6

#include <atomic>
#include <cinttypes>
#include <cstdio>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

constexpr gpio_num_t PIN_BOOT     = GPIO_NUM_0;
constexpr uint32_t   DEBOUNCE_US  = 30000;
constexpr uint32_t   HANDLING_MS  = 400;  // імітація довгої обробки в суперциклі
constexpr uint32_t   LOOP_IDLE_MS = 10;   // пауза суперциклу, щоб не голодувала задача IDLE

constexpr bool USE_EXCHANGE = true;

static std::atomic<bool> bootEvent{false};
static std::atomic<uint32_t> isrPresses{0};
static uint32_t lastAcceptedUs = 0;  // лише в ISR

static void IRAM_ATTR onBootPressed(void*) {
  const uint32_t nowUs = static_cast<uint32_t>(esp_timer_get_time());
  if (nowUs - lastAcceptedUs < DEBOUNCE_US) {
    return;
  }
  lastAcceptedUs = nowUs;

  isrPresses.fetch_add(1, std::memory_order_relaxed);
  bootEvent.store(true, std::memory_order_release);
}

static void initBootButton(gpio_isr_t handler) {
  const gpio_config_t config = {
      .pin_bit_mask = 1ULL << PIN_BOOT,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_NEGEDGE,
  };
  ESP_ERROR_CHECK(gpio_config(&config));
  ESP_ERROR_CHECK(gpio_install_isr_service(0));
  ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_BOOT, handler, nullptr));
}

static void handlePress(uint32_t handled) {
  printf("ISR нарахувала: %" PRIu32 "  оброблено: %" PRIu32 "\n",
         isrPresses.load(std::memory_order_relaxed), handled);
  vTaskDelay(pdMS_TO_TICKS(HANDLING_MS));
}

extern "C" void app_main() {
  initBootButton(onBootPressed);
  printf("\n=== Крок 6: %s. atomic<bool> lock-free: %s ===\n",
         USE_EXCHANGE ? "exchange(false)" : "if (flag) {...; flag = false;}",
         bootEvent.is_lock_free() ? "так" : "ні");

  uint32_t handled = 0;
  while (true) {
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
    vTaskDelay(pdMS_TO_TICKS(LOOP_IDLE_MS));
  }
}

#endif  // PRACTICE_STEP == 6
