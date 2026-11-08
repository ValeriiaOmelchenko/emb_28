// Крок 3. ISR рахує натискання BOOT: сирі фронти vs після debounce.
//
// Trust No Pin: одне натискання механічної кнопки - це кілька фронтів (брязкіт контактів).
// ISR рахує:
//   rawEdges        - кожен спадний фронт;
//   acceptedPresses - лише фронти, між якими пройшло ≥ DEBOUNCE_US.
// app_main() раз на PRINT_PERIOD_MS читає лічильники і друкує, якщо щось змінилось.

#if PRACTICE_STEP == 3

#include <cinttypes>
#include <cstdio>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

constexpr gpio_num_t PIN_BOOT        = GPIO_NUM_0;
constexpr uint32_t   DEBOUNCE_US     = 30000;  // 30 мс: довше за типовий брязкіт (1-10 мс)
constexpr uint32_t   PRINT_PERIOD_MS = 50;

// Пише ISR, читає app_main() → volatile.
// 32-бітне читання на 32-бітному ядрі - одна інструкція, тому «розірваним» воно не буде (див. крок 5).
static volatile uint32_t rawEdges = 0;
static volatile uint32_t acceptedPresses = 0;

// Потрібна лише всередині ISR → volatile не потрібен.
static uint32_t lastAcceptedUs = 0;

static void IRAM_ATTR onBootEdge(void*) {
  // Запис «x = x + 1», а не «++x»: інкремент volatile застарілий з C++20 (тут компілятор
  // дав би попередження) і нагадує, що це НЕ атомарна операція. Безпечно, бо пише лише одна ISR.
  rawEdges = rawEdges + 1;

  // esp_timer_get_time() дозволено викликати з ISR. Беззнакове віднімання переживає переповнення.
  const uint32_t nowUs = static_cast<uint32_t>(esp_timer_get_time());
  if (nowUs - lastAcceptedUs >= DEBOUNCE_US) {
    lastAcceptedUs = nowUs;
    acceptedPresses = acceptedPresses + 1;
  }
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

extern "C" void app_main() {
  initBootButton(onBootEdge);
  printf("\n=== Крок 3: натискай BOOT, порівнюй фронти і натискання ===\n");

  uint32_t shownEdges = 0;
  while (true) {
    // Знімок: читаємо кожну volatile один раз і далі працюємо з локальними копіями.
    const uint32_t edges = rawEdges;
    const uint32_t presses = acceptedPresses;

    if (edges != shownEdges) {
      shownEdges = edges;
      printf("фронтів: %" PRIu32 "  натискань після debounce: %" PRIu32 "\n", edges, presses);
    }

    vTaskDelay(pdMS_TO_TICKS(PRINT_PERIOD_MS));
  }
}

#endif  // PRACTICE_STEP == 3
