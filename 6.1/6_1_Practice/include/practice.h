#pragma once
// Спільне для всіх кроків: піни, інтервали з 2.3, розміри стеків, ініціалізація LED.
// Чистий C (C17, стандарт ESP-IDF за замовчуванням): процедурно, без динамічної памʼяті.
//
// Чому #define, а не const-змінна: у C змінна з const - це НЕ константний вираз.
// Її не можна взяти для розміру масиву чи в ініціалізатор static-таблиці (BLINKERS у кроці 2).
// Тому константи - #define з явним типом через UINT32_C() (дає суфікс під uint32_t,
// і printf з PRIu32 не свариться), складні вирази - у дужках.

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

// --- Залізо (ESP32-S3-DevKitC-1), як у 2.3 ---
// GPIO 4-6: не strapping (0, 3, 45, 46), не USB (19, 20), не UART0-консоль (43, 44),
// не флеш/PSRAM (26-37). Резистори від 3,3 В: червоний 270 Ом, зелений/синій 68 Ом.
#define LED_RED   GPIO_NUM_4
#define LED_GREEN GPIO_NUM_5
#define LED_BLUE  GPIO_NUM_6

// --- Інтервали з 2.3: час між перемиканнями ---
#define RED_TOGGLE_MS   UINT32_C(200)
#define GREEN_TOGGLE_MS UINT32_C(500)
#define BLUE_TOGGLE_MS  UINT32_C(1000)

// --- Стеки задач ---
// В ESP-IDF розмір стеку в xTaskCreate() задається в БАЙТАХ.
// У «ванільному» FreeRTOS (STM32, CMSIS) - у СЛОВАХ по 4 байти: те саме число 2048
// там означає 8 КБ. Переносиш код між платформами - перераховуй.
#define LED_STACK_BYTES   UINT32_C(2048)  // лише GPIO і vTaskDelay
#define PRINT_STACK_BYTES UINT32_C(4096)  // printf сам по собі зʼїдає 1,5-2 КБ стеку

// --- Пріоритети (більше число = важливіше) ---
// IDLE = 0. Задачі LED важливіші за звіт: звіт може почекати, блимання - ні.
#define PRIO_REPORT ((UBaseType_t)1)
#define PRIO_LED    ((UBaseType_t)2)

// Вихід з відомим рівнем на старті. Одна структура gpio_config_t замість кількох викликів.
// static inline: функція в заголовку, кожен .c отримує свою копію без конфлікту імен.
static inline void led_init(gpio_num_t pin) {
  const gpio_config_t conf = {
      .pin_bit_mask = 1ULL << pin,
      .mode         = GPIO_MODE_OUTPUT,
      .pull_up_en   = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type    = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&conf));
  ESP_ERROR_CHECK(gpio_set_level(pin, 0));
}

// xTaskCreate() мовчки повертає помилку, якщо не вистачило памʼяті під стек.
// Без перевірки LED просто «не блимає», і незрозуміло чому.
static inline void check_task_created(BaseType_t result) {
  configASSERT(result == pdPASS);
}
