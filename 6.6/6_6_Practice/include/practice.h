#pragma once
// Спільне для кроків практики 6.6: кнопка пробудження, очищення UART перед сном, перевірки.
// Чистий C (C17, стандарт ESP-IDF за замовчуванням): процедурно, без динамічної памʼяті.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

// --- Кнопка пробудження: між GPIO4 і GND ---
// GPIO4 - RTC GPIO (на ESP32-S3 це GPIO 0-21), тому годиться і для EXT0 у Deep Sleep.
// Кнопку BOOT (GPIO0) навмисно не беремо: це strapping-пін, у виробі на ньому датчик не вішають.
// Trust No Pin: внутрішня підтяжка ~45 кОм слабка, для довгого дроту або виробу - зовнішній 10 кОм до 3V3.
#define BUTTON_PIN            GPIO_NUM_0
#define BUTTON_PRESSED_LEVEL  0
#define BUTTON_RELEASED_LEVEL 1

#define DEBOUNCE_MS          UINT32_C(30)   // скільки «відпущено» має триматися стабільно
#define DEBOUNCE_POLL_MS     UINT32_C(10)
#define RELEASE_TIMEOUT_MS   UINT32_C(3000) // залипла кнопка не має тримати чип без сну вічно

// Кнопка як звичайний цифровий вхід з підтяжкою, без переривань.
// gpio_config() також повертає пін з RTC-мультиплексора (після EXT0) у цифровий.
static inline void button_init(void) {
  const gpio_config_t io_conf = {
      .pin_bit_mask = 1ULL << BUTTON_PIN,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&io_conf));
}

// Чекає, поки кнопка буде стабільно відпущена DEBOUNCE_MS. Брязкіт скидає відлік.
// false - кнопку тримають (або вона залипла) довше за RELEASE_TIMEOUT_MS.
// Навіщо: пробудження за РІВНЕМ. Засни з натиснутою кнопкою - і чип одразу прокинеться знову.
static inline bool wait_button_released(void) {
  uint32_t stable_ms = 0;
  for (uint32_t waited_ms = 0; waited_ms < RELEASE_TIMEOUT_MS; waited_ms += DEBOUNCE_POLL_MS) {
    stable_ms = (gpio_get_level(BUTTON_PIN) == BUTTON_RELEASED_LEVEL) ? stable_ms + DEBOUNCE_POLL_MS : 0;
    if (stable_ms >= DEBOUNCE_MS) {
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_POLL_MS));
  }
  return false;
}

// Дочекатися, поки останній рядок логу фізично вийде з UART, і лише потім спати.
// Інакше хвіст повідомлення обріжеться або перетвориться на сміття в моніторі.
static inline void console_flush(void) {
  fflush(stdout);
#if CONFIG_ESP_CONSOLE_UART
  uart_wait_tx_idle_polling((uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM);
#endif
}

// xTaskCreate() повертає pdPASS або помилку (не вистачило heap для стеку).
static inline void check_task_created(BaseType_t result, const char *name) {
  if (result != pdPASS) {
    ESP_LOGE("practice", "Не вдалося створити задачу %s (мало heap?)", name);
    abort();
  }
}
