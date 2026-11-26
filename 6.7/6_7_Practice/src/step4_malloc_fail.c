// Крок 4: malloc(25 КБ) повертає NULL, хоча вільно сотні КБ. Спрацьовує callback невдалого виділення.
// Нічого підключати не треба.
//
// Heap фрагментуємо тим самим способом, що в кроці 3 (include/fragment.h), але без графіка.
// Потім:
//   malloc(12 КБ) - є дірка такого розміру, виділяється;
//   malloc(25 КБ) - жодна дірка не має 25 КБ підряд, NULL, хоча free > 100 КБ.
//
// vApplicationMallocFailedHook() з методички в ESP-IDF не викликається: тут pvPortMalloc()
// - це обгортка над heap_caps_malloc(), а configUSE_MALLOC_FAILED_HOOK = 0.
// Аналог в ESP-IDF - heap_caps_register_failed_alloc_callback(): спрацьовує на КОЖНЕ
// невдале виділення (malloc, calloc, pvPortMalloc, heap_caps_*). Опція
// CONFIG_HEAP_ABORT_WHEN_ALLOCATION_FAILS робить з цього одразу abort().

#include <assert.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fragment.h"
#include "practice.h"

#define SMALL_REQUEST_B  (UINT32_C(12) * BYTES_PER_KB) // є дірка такого розміру
#define BIG_REQUEST_B    (UINT32_C(25) * BYTES_PER_KB) // як на слайді: 25 КБ при дірках <= 20 КБ
#define PLOT_PERIOD_MS   UINT32_C(1000)

static_assert(BIG_REQUEST_B > FRAG_BLOCK_MAX_B, "запит має бути більшим за найбільшу дірку");
static_assert(SMALL_REQUEST_B <= FRAG_BLOCK_MAX_B, "малий запит має влазити в дірку");

static const char *TAG = "fail";

static frag_state_t s_frag;
static atomic_uint s_failed_allocs; // лічильник, який можна читати з будь-якої задачі

// Викликається з контексту того, хто кликав malloc, у момент невдачі.
// Тут не можна нічого виділяти (heap щойно відмовив) і краще не блокуватись:
// лише лічильник і короткий ROM-друк без heap і без мʼютексів логера.
static void on_alloc_failed(size_t size, uint32_t caps, const char *function_name) {
  atomic_fetch_add(&s_failed_allocs, 1U);
  esp_rom_printf("!!! alloc failed: %s(%u B), caps 0x%08x\n", function_name, (unsigned)size, (unsigned)caps);
}

// Як поводиться код, коли malloc() повернув NULL. Тут - soft fail: лог і пропуск операції.
// Для важливої задачі - safe-mode, для критичної - configASSERT() і перезапуск через watchdog.
static void try_malloc(size_t bytes) {
  ESP_LOGI(TAG, "malloc(%zu): free %" PRIu32 " B, largest %zu B", bytes, esp_get_free_heap_size(),
           heap_caps_get_largest_free_block(HEAP_CAPS_MALLOC));
  uint8_t *buf = malloc(bytes);
  if (buf == NULL) {
    ESP_LOGE(TAG, "malloc(%zu) = NULL - операцію пропущено", bytes);
    return;
  }
  memset(buf, 0, bytes); // справді користуємось памʼяттю
  ESP_LOGI(TAG, "malloc(%zu) = %p - ок", bytes, (void *)buf);
  free(buf);
}

void app_main(void) {
  log_heap(TAG, "старт");
  try_malloc(BIG_REQUEST_B); // на чистому heap 25 КБ - не проблема

  // Під час заповнення fill сам отримує NULL - це очікувано, тому callback реєструємо після.
  frag_fill(&s_frag, false);
  frag_free_every_second(&s_frag, false);
  ESP_ERROR_CHECK(heap_caps_register_failed_alloc_callback(on_alloc_failed));
  log_heap(TAG, "після фрагментації");

  if (heap_caps_get_largest_free_block(HEAP_CAPS_MALLOC) >= BIG_REQUEST_B) {
    ESP_LOGW(TAG, "фрагментація не вийшла: є шматок >= 25 КБ (інша конфігурація heap?)");
  }

  try_malloc(SMALL_REQUEST_B);
  try_malloc(BIG_REQUEST_B);
  ESP_LOGW(TAG, "невдалих виділень (callback): %u", atomic_load(&s_failed_allocs));

  // Той самий запит після прибирання знову проходить: справа не в обсязі, а в суцільності.
  frag_release_all(&s_frag);
  try_malloc(BIG_REQUEST_B);

  for (;;) {
    plot_heap();
    vTaskDelay(pdMS_TO_TICKS(PLOT_PERIOD_MS));
  }
}
