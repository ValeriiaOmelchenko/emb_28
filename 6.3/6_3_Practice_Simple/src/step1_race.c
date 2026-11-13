// Крок 1: стан гонки (race condition) на спільному лічильнику.
//
// Дві задачі на ДВОХ ядрах одночасно роблять s_counter++ по ITERATIONS разів.
// Очікуємо 2 * ITERATIONS, отримуємо менше.
// Причина: ++ - це три дії (прочитати / додати / записати). Якщо обидва ядра прочитали
// те саме значення, один з інкрементів губиться.
// Аналогія: двоє касирів одночасно бачать у зошиті залишок «10», кожен продає
// по товару і записує «9». Продано два, а в зошиті мінус один.
//
// Повторити - кнопка RST на платі: число щоразу інше.
//
// Експеримент з _Atomic: постав USE_ATOMIC 1. Рядок s_counter++ лишається тим самим,
// змінюється лише тип змінної - і втрат більше немає. Ціна видно в лозі: час роботи зростає.

#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "practice.h"

#define ITERATIONS  UINT32_C(1000000)  // десятки мс роботи: задачі гарантовано перетинаються
#define TASK_A_CORE 0
#define TASK_B_CORE 1                  // експеримент: постав 0 - обидві задачі на одному ядрі
// Пріоритет як у app_main (1): створена задача не витісняє app_main, і та встигає
// створити обидві. А якщо обидві на одному ядрі - вони ділять його по черзі кожен тік.
#define PRIO_WORKER ((UBaseType_t)1)
#define USE_ATOMIC  0  // експеримент: 1 - лічильник _Atomic, гонки немає

static const char *TAG = "step1_race";

#if USE_ATOMIC
// _Atomic: компілятор сам перетворює s_counter++ на atomic_fetch_add - неподільне
// «прочитати-додати-записати». На S3 це цикл з інструкцією S32C1I (compare-and-swap):
// якщо інше ядро встигло змінити значення, спроба повторюється.
static _Atomic uint32_t s_counter;
#define COUNTER_KIND "_Atomic"
#else
// volatile тут НАВМИСНО: «щоразу читай з памʼяті», але атомарності НЕ дає.
// Саме це й показуємо: volatile від гонки не захищає.
static volatile uint32_t s_counter;
#define COUNTER_KIND "volatile"
#endif

// Скільки задач закінчили. _Atomic: atomic_fetch_add - неподільне «прочитати-додати-записати».
// Окрема змінна, а не s_counter: дані і прапорець «готово» не змішуємо.
static _Atomic uint32_t s_done;

static void increment_task(void *arg) {
  (void)arg;
  // Ні логів перед циклом: лог займає UART на мілісекунди, і задачі розійшлися б у часі.
  for (uint32_t i = 0; i < ITERATIONS; ++i) {
    s_counter++;  // volatile: прочитати -> додати -> записати, НЕ атомарно; _Atomic - атомарно
  }
  atomic_fetch_add(&s_done, 1);
  vTaskDelete(NULL);  // задача FreeRTOS не може просто повернутися з функції
}

void app_main(void) {
  ESP_LOGI(TAG, "=== Гонка: 2 задачі x %" PRIu32 " інкрементів, ядра %d і %d, лічильник %s ===",
           ITERATIONS, TASK_A_CORE, TASK_B_CORE, COUNTER_KIND);

  const int64_t start_us = esp_timer_get_time();
  check_task_created(xTaskCreatePinnedToCore(increment_task, "inc_A", TASK_STACK_BYTES, NULL,
                                             PRIO_WORKER, NULL, TASK_A_CORE),
                     "inc_A");
  check_task_created(xTaskCreatePinnedToCore(increment_task, "inc_B", TASK_STACK_BYTES, NULL,
                                             PRIO_WORKER, NULL, TASK_B_CORE),
                     "inc_B");

  // Чекаємо обидві задачі. vTaskDelay - не busy-wait: ядро 0 віддаємо задачі inc_A.
  // Перевіряємо кожен тік (1 мс), щоб час роботи в лозі був точним до мілісекунди.
  while (atomic_load(&s_done) < 2) {
    vTaskDelay(1);
  }
  const uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);

  const uint32_t expected = 2 * ITERATIONS;
  const uint32_t actual = s_counter;
  // actual > expected тут неможливий (інкременти лише губляться), тож різниця не переповниться.
  ESP_LOGW(TAG, "Лічильник %" PRIu32 " з %" PRIu32 ", втрачено %" PRIu32 ", час %" PRIu32 " мс",
           actual, expected, expected - actual, elapsed_ms);
  ESP_LOGI(TAG, "Порівняй volatile і _Atomic (USE_ATOMIC). Повторити - RST.");
}
