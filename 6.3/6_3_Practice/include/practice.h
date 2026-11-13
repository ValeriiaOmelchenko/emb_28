#pragma once
// Спільне для обох кроків практики 6.3: стеки, пріоритети, перевірка створення задач.
// Чистий C (C17, стандарт ESP-IDF за замовчуванням): процедурно, без динамічної памʼяті в циклі.
//
// Константи - #define з явним типом: у C const-змінна не є константним виразом
// (не годиться для розміру масиву, static_assert і ініціалізатора static-таблиці).

#include <stdint.h>
#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

// --- Стеки задач ---
// В ESP-IDF розмір стеку в xTaskCreate() - у БАЙТАХ (у «ванільному» FreeRTOS - у словах).
// ESP_LOGx / printf самі по собі зʼїдають 1,5-2 КБ стеку, тому 2048 для задачі з логом - впритул.
// Реальний запас перевіряємо uxTaskGetStackHighWaterMark() (в ESP-IDF теж у байтах).
#define LOG_TASK_STACK_BYTES UINT32_C(3072)

// --- Пріоритети (більше число = важливіше). app_main працює з пріоритетом 1. ---
#define PRIO_WORKER   ((UBaseType_t)5)
#define PRIO_CONSUMER ((UBaseType_t)10)
// На час створення задач app_main тимчасово важливіша за всіх: інакше задача на її ядрі
// витіснила б app_main одразу після створення. Потім app_main повертає собі пріоритет 1.
#define PRIO_INIT     ((UBaseType_t)(PRIO_CONSUMER + 1))

// xTaskCreate*() повертає pdPASS або помилку (не вистачило heap для стеку).
// Без перевірки «задача мовчить» не відрізнити від «задача не створилась».
static inline void check_task_created(BaseType_t result, const char *name) {
  if (result != pdPASS) {
    ESP_LOGE("practice", "Не вдалося створити задачу %s (мало heap?)", name);
    abort();
  }
}
