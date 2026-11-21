#pragma once
// Спільне для всіх кроків практики 6.5: ядра, стеки, перевірка створення задач, «зайнятий» CPU.
// Чистий C (C17, стандарт ESP-IDF за замовчуванням): процедурно, без динамічної памʼяті в циклі.
//
// Константи - #define з явним типом: у C const-змінна не є константним виразом
// (не годиться для розміру масиву, static_assert і ініціалізатора static-таблиці).

#include <stdint.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_FREERTOS_UNICORE
#error "Практика показує паралелізм двох ядер: menuconfig -> FreeRTOS -> Run FreeRTOS only on first core = вимкнено"
#endif

// --- Ядра ESP32-S3 (два Xtensa LX7). Тип - як в останньому параметрі xTaskCreatePinnedToCore(). ---
// app_main працює на ядрі 0 з пріоритетом 1.
#define CORE_0 ((BaseType_t)0)
#define CORE_1 ((BaseType_t)1)

// --- Стеки задач (в ESP-IDF - у БАЙТАХ, не в словах, як у ванільному FreeRTOS) ---
// ESP_LOGx сам по собі зʼїдає 1,5-2 КБ стеку, тому для задач з логом - 3 КБ.
#define TASK_STACK_BYTES UINT32_C(3072)

// xTaskCreate() повертає pdPASS або помилку (не вистачило heap для стеку).
// Без перевірки «задача мовчить» не відрізнити від «задача не створилась».
static inline void check_task_created(BaseType_t result, const char *name) {
  if (result != pdPASS) {
    ESP_LOGE("practice", "Не вдалося створити задачу %s (мало heap?)", name);
    abort();
  }
}

// Імітація роботи, яка ЗАЙМАЄ процесор (обчислення, обробка буфера).
// На відміну від vTaskDelay(), задача не засинає: поки вона «працює», інші задачі
// з нижчим пріоритетом на цьому ядрі не виконуються. Шматками по 1 мс, щоб робота
// рахувалась у часі CPU: якщо задачу витіснили, решта шматків чекає її повернення.
static inline void busy_work_ms(uint32_t ms) {
  for (uint32_t i = 0; i < ms; i++) {
    esp_rom_delay_us(1000);
  }
}
