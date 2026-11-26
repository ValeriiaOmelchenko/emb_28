// Крок 3: навмисна фрагментація різнорозмірними heap_caps_malloc()/heap_caps_free().
// Нічого підключати не треба. Відкрий Serial Plotter / Teleplot: free_kb і largest_kb.
//
// Що побачимо:
//   фаза «заповнення»  - free і largest падають разом, розриву немає;
//   фаза «звільнення»  - free росте з кожним free(), а largest стоїть на ~20 КБ:
//                        розрив між лініями і є фрагментація;
//   фаза «прибирання»  - звільнили сусідів, алокатор злив дірки, largest повернувся.
// Механіка - у include/fragment.h.

#include <inttypes.h>
#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fragment.h"
#include "practice.h"

#define PHASE_PAUSE_MS   UINT32_C(3000)
#define HOLD_PLOTS       UINT32_C(10) // скільки секунд тримаємо фрагментований стан на графіку
#define PLOT_PERIOD_MS   UINT32_C(1000)

static const char *TAG = "frag";

static frag_state_t s_frag; // static: ~1,3 КБ вказівників не на стеку main

static void hold_and_plot(uint32_t seconds) {
  for (uint32_t i = 0; i < seconds; i++) {
    plot_heap();
    vTaskDelay(pdMS_TO_TICKS(PLOT_PERIOD_MS));
  }
}

void app_main(void) {
  log_heap(TAG, "старт");
  vTaskDelay(pdMS_TO_TICKS(PHASE_PAUSE_MS));

  ESP_LOGI(TAG, "--- заповнення: блоки 8-20 КБ упритул ---");
  frag_fill(&s_frag, true);
  ESP_LOGI(TAG, "виділено %" PRIu32 " блоків і %" PRIu32 " пробок по %" PRIu32 " B", s_frag.block_count,
           s_frag.plug_count, FRAG_PLUG_BYTES);
  log_heap(TAG, "heap заповнений");
  vTaskDelay(pdMS_TO_TICKS(PHASE_PAUSE_MS));

  ESP_LOGI(TAG, "--- звільнення кожного другого блоку ---");
  frag_free_every_second(&s_frag, true);
  log_heap(TAG, "фрагментовано");
  ESP_LOGW(TAG, "вільно %" PRIu32 " КБ, але найбільший шматок лише %" PRIu32 " КБ",
           esp_get_free_heap_size() / BYTES_PER_KB,
           (uint32_t)heap_caps_get_largest_free_block(HEAP_CAPS_MALLOC) / BYTES_PER_KB);
  hold_and_plot(HOLD_PLOTS);

  ESP_LOGI(TAG, "--- прибирання: звільняємо сусідів ---");
  frag_release_all(&s_frag);
  log_heap(TAG, "усе звільнено");

  for (;;) {
    hold_and_plot(HOLD_PLOTS);
  }
}
