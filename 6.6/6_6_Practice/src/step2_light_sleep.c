// Крок 2: Light Sleep вручну. Таймер (5 с) або кнопка на GPIO4 будять чип.
// Підключення: кнопка між GPIO4 і GND (див. README).
//
// Що побачимо:
//   - струм падає з десятків мА до одиниць мА (на DevKit) на час сну;
//   - після esp_light_sleep_start() виконання йде з НАСТУПНОГО рядка тієї ж функції:
//     локальні змінні на стеку, static-змінні, стан FreeRTOS - все на місці;
//   - причину пробудження можна дізнатися і тут, не лише після Deep Sleep.
//
// Аналогія: Light Sleep - людина задрімала в кріслі з книжкою в руках.
// Прокинулась - читає з того ж рядка. Deep Sleep (крок 3) - лягла спати, книжку закрила.

#include <inttypes.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "practice.h"

#define SLEEP_MS UINT64_C(5000)

static const char *TAG = "light";

static void wake_sources_init(void) {
  ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(SLEEP_MS * 1000)); // в мікросекундах
  // GPIO-пробудження в Light Sleep: лише за РІВНЕМ (LOW/HIGH), не за фронтом.
  ESP_ERROR_CHECK(gpio_wakeup_enable(BUTTON_PIN, GPIO_INTR_LOW_LEVEL));
  ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup());
}

void app_main(void) {
  button_init();
  wake_sources_init();

  // Локальна змінна app_main (це теж задача FreeRTOS) - живе на її стеку.
  uint32_t wake_no = 0;

  for (;;) {
    ESP_LOGI(TAG, "#%" PRIu32 ": засинаю на %" PRIu64 " мс (кнопка GPIO4 - раніше)", wake_no, SLEEP_MS);
    console_flush();

    const int64_t t0_us = esp_timer_get_time();
    esp_light_sleep_start(); // тут CPU зупинено, тактування вимкнено, RAM під живленням
    const int64_t slept_ms = (esp_timer_get_time() - t0_us) / 1000;

    wake_no++;
    const uint32_t causes = esp_sleep_get_wakeup_causes(); // бітова маска BIT(ESP_SLEEP_WAKEUP_x)
    const char *by = (causes & BIT(ESP_SLEEP_WAKEUP_GPIO))    ? "кнопка"
                     : (causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) ? "таймер"
                                                              : "інше";
    ESP_LOGI(TAG, "#%" PRIu32 ": прокинулась через %" PRId64 " мс, причина: %s. wake_no не загубився", wake_no,
             slept_ms, by);

    if ((causes & BIT(ESP_SLEEP_WAKEUP_GPIO)) && !wait_button_released()) {
      // Рівень LOW досі тримається: засни зараз - і миттю прокинешся знову.
      ESP_LOGW(TAG, "кнопка затиснута > %" PRIu32 " мс - сплю лише за таймером", RELEASE_TIMEOUT_MS);
      ESP_ERROR_CHECK(gpio_wakeup_disable(BUTTON_PIN));
    } else {
      ESP_ERROR_CHECK(gpio_wakeup_enable(BUTTON_PIN, GPIO_INTR_LOW_LEVEL));
    }
  }
}
