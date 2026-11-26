#pragma once
// Спільне для кроків практики 6.7: вивід стану heap, перевірка створення задач, «баласт» heap.
// Чистий C (C17, стандарт ESP-IDF за замовчуванням): процедурно.
//
// Константи - #define з явним типом: у C const-змінна не є константним виразом
// (не годиться для розміру масиву, static_assert і ініціалізатора static-таблиці).

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define BYTES_PER_KB UINT32_C(1024)

// Звідки бере памʼять звичайний malloc(). Саме ці регіони рахує esp_get_free_heap_size(),
// тому найбільший блок міряємо з тими самими прапорами - порівнюємо яблука з яблуками.
#define HEAP_CAPS_MALLOC ((uint32_t)MALLOC_CAP_DEFAULT)

// Короткий стан heap у лог (людям).
//   free     - сумарно вільно по всіх шматках (esp_get_free_heap_size);
//   largest  - найбільший СУЦІЛЬНИЙ шматок: більший malloc() гарантовано поверне NULL;
//   min ever - найменше значення free за весь час роботи (low water mark heap).
static inline void log_heap(const char *tag, const char *label) {
  ESP_LOGI(tag, "%-28s free %6" PRIu32 " B | largest %6zu B | min ever %6" PRIu32 " B", label,
           esp_get_free_heap_size(), heap_caps_get_largest_free_block(HEAP_CAPS_MALLOC),
           esp_get_minimum_free_heap_size());
}

// Той самий стан для Serial Plotter / Teleplot: формат name:value name2:value2, у КБ.
static inline void plot_heap(void) {
  printf("free_kb:%" PRIu32 " largest_kb:%" PRIu32 " min_free_kb:%" PRIu32 "\n",
         esp_get_free_heap_size() / BYTES_PER_KB,
         (uint32_t)heap_caps_get_largest_free_block(HEAP_CAPS_MALLOC) / BYTES_PER_KB,
         esp_get_minimum_free_heap_size() / BYTES_PER_KB);
}

// xTaskCreate() повертає pdPASS або errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY (не вистачило heap).
// Без перевірки «задача мовчить» не відрізнити від «задача не створилась».
static inline void check_task_created(BaseType_t result, const char *name) {
  if (result != pdPASS) {
    ESP_LOGE("practice", "Не вдалося створити задачу %s (мало heap?)", name);
    abort();
  }
}

// High Water Mark у відсотках від розміру стеку.
// В ESP-IDF і розмір стеку в xTaskCreate(), і uxTaskGetStackHighWaterMark() - у БАЙТАХ
// (у ванільному FreeRTOS - у словах StackType_t, на 32-біт МК це x4).
// Орієнтири спільноти (не специфікація FreeRTOS): > 20% - ок, < 10% - збільшити стек, 0 - переповнення.
static inline uint32_t stack_free_pct(UBaseType_t hwm_bytes, uint32_t stack_bytes) {
  return (uint32_t)hwm_bytes * UINT32_C(100) / stack_bytes;
}

// --- «Баласт»: імітуємо, що решта програми (Wi-Fi, BLE, драйвери) вже зʼїла майже весь heap ---
// Забираємо найбільші шматки, поки вільним не лишиться приблизно leave_bytes.
// Навмисно через heap: це інструмент демонстрації, а не зразок для виробу.
#define BALLAST_MAX_CHUNKS   UINT32_C(32)
#define BALLAST_MIN_CHUNK_B  UINT32_C(64)

typedef struct {
  void *chunks[BALLAST_MAX_CHUNKS];
  uint32_t count;
} heap_ballast_t;

static inline void heap_ballast_take(heap_ballast_t *b, uint32_t leave_bytes) {
  while (b->count < BALLAST_MAX_CHUNKS) {
    const uint32_t free_b = esp_get_free_heap_size();
    if (free_b <= leave_bytes) {
      break;
    }
    uint32_t take_b = (uint32_t)heap_caps_get_largest_free_block(HEAP_CAPS_MALLOC);
    if (take_b > free_b - leave_bytes) {
      take_b = free_b - leave_bytes;
    }
    if (take_b < BALLAST_MIN_CHUNK_B) {
      break;
    }
    void *p = heap_caps_malloc(take_b, HEAP_CAPS_MALLOC);
    if (p == NULL) {
      break; // службові байти блоку не влізли - для баласту цього досить
    }
    b->chunks[b->count++] = p;
  }
}

static inline void heap_ballast_release(heap_ballast_t *b) {
  for (uint32_t i = 0; i < b->count; i++) {
    heap_caps_free(b->chunks[i]);
    b->chunks[i] = NULL;
  }
  b->count = 0;
}
