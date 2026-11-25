// Крок 1: Active. Обидва ядра на 240 МГц зайняті обчисленнями - вимірюємо струм.
// Нічого підключати не треба.
//
// BUSY_CORES - скільки ядер «молотять» (змінити, прошити знову, порівняти струм):
//   2 - обидва ядра рахують без зупинки (максимум без радіо);
//   1 - лише ядро 0;
//   0 - задачі весь час у Blocked (vTaskDelay). Без Power Management процесор
//       при цьому НЕ спить: тактування 240 МГц лишається, ядро просто чекає
//       переривання (інструкція WAITI). Струм нижчий, але це ще не Light Sleep.
//
// Wi-Fi не вмикаємо: з радіо пікові струми на порядок вищі (див. README).

#include <inttypes.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "practice.h"

#define BUSY_CORES 2

#define REPORT_PERIOD_MS UINT32_C(1000)
#define TASK_STACK_BYTES UINT32_C(3072)
#define PRIO_WORKER      ((UBaseType_t)1)

static const char *TAG = "active";

// «Корисна робота»: шматок обчислень, який компілятор не може викинути.
static uint32_t crunch(uint32_t x) {
  for (uint32_t i = 0; i < UINT32_C(10000); i++) {
    x = x * UINT32_C(1664525) + UINT32_C(1013904223); // LCG, просто щоб CPU рахував
  }
  return x;
}

static void worker_task(void *arg) {
  const bool busy = (bool)(uintptr_t)arg;
  uint32_t seed = 1;
  for (;;) {
    if (!busy) {
      vTaskDelay(pdMS_TO_TICKS(REPORT_PERIOD_MS)); // Blocked: CPU вільний
      ESP_LOGI(TAG, "ядро %d: чекаю (Blocked)", xPortGetCoreID());
      continue;
    }
    // Рахуємо рівно секунду, потім віддаємо 1 тік Idle Task.
    // Без цієї паузи Idle голодує, і через 5 с спрацює Task Watchdog.
    uint32_t rounds = 0;
    const int64_t until_us = esp_timer_get_time() + (int64_t)REPORT_PERIOD_MS * 1000;
    while (esp_timer_get_time() < until_us) {
      seed = crunch(seed);
      rounds++;
    }
    ESP_LOGI(TAG, "ядро %d: %" PRIu32 " раундів за %" PRIu32 " мс (seed %08" PRIx32 ")", xPortGetCoreID(),
             rounds, REPORT_PERIOD_MS, seed);
    vTaskDelay(1);
  }
}

void app_main(void) {
  ESP_LOGI(TAG, "CPU %d МГц, ядер зайнято: %d. Дивись на мультиметр.", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
           BUSY_CORES);

  for (BaseType_t core = 0; core < 2; core++) {
    const bool busy = core < BUSY_CORES;
    check_task_created(xTaskCreatePinnedToCore(worker_task, "worker", TASK_STACK_BYTES, (void *)(uintptr_t)busy,
                                               PRIO_WORKER, NULL, core),
                       "worker");
  }
}
