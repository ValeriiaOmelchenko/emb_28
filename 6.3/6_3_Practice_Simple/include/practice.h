#pragma once
// Спільне для всіх кроків практики 6.3. Чистий C (C17, стандарт ESP-IDF за замовчуванням).

#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

// В ESP-IDF розмір стеку в xTaskCreate() - у БАЙТАХ (у «ванільному» FreeRTOS - у словах).
// ESP_LOGx зʼїдає 1,5-2 КБ стеку, тому 2048 для задачі з логом - впритул.
#define TASK_STACK_BYTES UINT32_C(3072)

// xTaskCreate*() повертає pdPASS або помилку (не вистачило heap для стеку).
// Без перевірки «задача мовчить» не відрізнити від «задача не створилась».
static inline void check_task_created(BaseType_t result, const char *name) {
  if (result != pdPASS) {
    ESP_LOGE("practice", "Не вдалося створити задачу %s", name);
    abort();
  }
}
