// Крок 1: моніторинг памʼяті на старті системи.
// Нічого підключати не треба.
//
// Що друкуємо:
//   - esp_get_free_heap_size()            - скільки вільно СУМАРНО;
//   - heap_caps_get_largest_free_block()  - найбільший СУЦІЛЬНИЙ шматок (індикатор фрагментації);
//   - esp_get_minimum_free_heap_size()    - найгірший момент за весь час роботи;
//   - heap_caps_print_heap_info()         - дамп усіх регіонів heap;

//   - uxTaskGetStackHighWaterMark()       - скільки стеку задачі ще жодного разу не торкались.
// Далі раз на секунду рядок для Serial Plotter, раз на 5 с - HWM обох задач.

#include <inttypes.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "practice.h"

#define REPORT_PERIOD_MS    UINT32_C(1000)
#define HWM_EVERY_REPORTS   UINT32_C(5)
#define WORKER_PERIOD_MS    UINT32_C(100)
#define WORKER_LOG_EVERY    UINT32_C(20)   // лог раз на 2 с
#define WORKER_STACK_BYTES  UINT32_C(4096) // ESP-IDF: у байтах
#define MAIN_STACK_BYTES    ((uint32_t)CONFIG_ESP_MAIN_TASK_STACK_SIZE)
#define PRIO_WORKER         ((UBaseType_t)2)

static const char *TAG = "monitor";

static TaskHandle_t s_worker;

// Задача, яка майже нічого не робить. Але лог з форматуванням - один з найбільших
// споживачів стеку: HWM покаже, що «порожня» задача з ESP_LOGI вже зʼїла 1,5-2 КБ.
static void worker_task(void *arg) {
  (void)arg;
  for (uint32_t tick = 1;; tick++) {
    if ((tick % WORKER_LOG_EVERY) == 0) {
      ESP_LOGI(TAG, "worker: тік %" PRIu32, tick);
    }
    vTaskDelay(pdMS_TO_TICKS(WORKER_PERIOD_MS));
  }
}

static void print_startup_snapshot(void) {
  ESP_LOGI(TAG, "=== Heap на старті ===");
  ESP_LOGI(TAG, "всього в heap (MALLOC_CAP_DEFAULT)     %7zu B", heap_caps_get_total_size(HEAP_CAPS_MALLOC));
  ESP_LOGI(TAG, "esp_get_free_heap_size()               %7" PRIu32 " B", esp_get_free_heap_size());
  ESP_LOGI(TAG, "heap_caps_get_largest_free_block()     %7zu B",
           heap_caps_get_largest_free_block(HEAP_CAPS_MALLOC));
  ESP_LOGI(TAG, "esp_get_minimum_free_heap_size()       %7" PRIu32 " B", esp_get_minimum_free_heap_size());
  ESP_LOGI(TAG, "--- за регіонами (heap_caps_get_free_size) ---");
  ESP_LOGI(TAG, "MALLOC_CAP_INTERNAL                    %7zu B", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  ESP_LOGI(TAG, "MALLOC_CAP_DMA                         %7zu B", heap_caps_get_free_size(MALLOC_CAP_DMA));
  ESP_LOGI(TAG, "MALLOC_CAP_SPIRAM                      %7zu B (PSRAM тут не ввімкнено, див. крок 6)",
           heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  // Детальний дамп: кожен регіон, скільки зайнято/вільно, найбільший блок, кількість блоків.
  heap_caps_print_heap_info(HEAP_CAPS_MALLOC);
}

static void print_stacks(void) {
  const UBaseType_t main_hwm = uxTaskGetStackHighWaterMark(NULL); // NULL = поточна задача (main)
  const UBaseType_t worker_hwm = uxTaskGetStackHighWaterMark(s_worker);
  ESP_LOGI(TAG, "HWM main:   %4u B вільно з %" PRIu32 " (%" PRIu32 "%%)", (unsigned)main_hwm, MAIN_STACK_BYTES,
           stack_free_pct(main_hwm, MAIN_STACK_BYTES));
  ESP_LOGI(TAG, "HWM worker: %4u B вільно з %" PRIu32 " (%" PRIu32 "%%)", (unsigned)worker_hwm, WORKER_STACK_BYTES,
           stack_free_pct(worker_hwm, WORKER_STACK_BYTES));
}

void app_main(void) {
  print_startup_snapshot();

  log_heap(TAG, "до xTaskCreate(worker)");
  check_task_created(xTaskCreate(worker_task, "worker", WORKER_STACK_BYTES, NULL, PRIO_WORKER, &s_worker), "worker");
  log_heap(TAG, "після xTaskCreate(worker)"); // мінус стек + TCB: задача теж живе в heap

  for (uint32_t report = 1;; report++) {
    vTaskDelay(pdMS_TO_TICKS(REPORT_PERIOD_MS));
    plot_heap(); // лінії рівні: у програмі нічого не виділяється в циклі - так і має бути
    if ((report % HWM_EVERY_REPORTS) == 0) {
      print_stacks();
    }
  }
}
