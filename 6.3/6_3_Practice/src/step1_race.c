// Крок 1: стан гонки (race condition) на спільному лічильнику.
//
// Дві однакові задачі на ДВОХ ядрах ESP32-S3 одночасно роблять shared_counter++
// по ITERATIONS разів. Очікуємо 2 * ITERATIONS, отримуємо менше.
// Причина: ++ - це три інструкції (прочитати / додати / записати). Якщо обидва ядра
// прочитали те саме значення, один з інкрементів губиться.
// Аналогія: двоє касирів одночасно дивляться на залишок у зошиті «10», кожен продає
// по одному товару і записує «9». Продано два, а в зошиті мінус один.
//
// Повторити експеримент - кнопка RST на платі: число щоразу інше.
// Експеримент на уроці: TASK_B_CORE = 0 (обидві задачі на одному ядрі).
// Втрат стає менше, але вони є: гонці не потрібні два ядра, досить того, що планувальник
// витіснить задачу між «прочитати» і «записати».

#include <assert.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "practice.h"

#define ITERATIONS    UINT32_C(1000000)  // ~десятки мс роботи: задачі гарантовано перетинаються
#define TASK_A_CORE   0
#define TASK_B_CORE   1                  // постав 0, щоб обидві задачі були на одному ядрі
#define POLL_PERIOD   pdMS_TO_TICKS(10)  // як часто app_main перевіряє, чи задачі закінчили
#define DONE_TIMEOUT  pdMS_TO_TICKS(5000)

static_assert(ITERATIONS <= UINT32_MAX / 2, "2 * ITERATIONS має влазити в uint32_t");

static const char *TAG = "step1_race";

// volatile тут НАВМИСНО: він каже компілятору «щоразу читай з памʼяті», але НЕ робить
// читання-зміну-запис атомарним. Саме це й показуємо: volatile від гонки не захищає.
static volatile uint32_t s_shared_counter;

// Скільки задач уже закінчили. А тут - _Atomic: atomic_fetch_add робить
// «прочитати-додати-записати» однією неподільною операцією, навіть з двох ядер.
// Контраст з s_shared_counter: той самий ++, але захищений.
static _Atomic uint32_t s_tasks_done;

typedef struct {
  const char *name;
  BaseType_t  core;
} race_task_config_t;

// Одна функція задачі на двох - параметри в таблиці, без копіпасти.
static const race_task_config_t TASKS[] = {
    {"inc_A", TASK_A_CORE},
    {"inc_B", TASK_B_CORE},
};
#define TASK_COUNT (sizeof(TASKS) / sizeof(TASKS[0]))

static void increment_task(void *arg) {
  (void)arg;
  // Ні логів, ні затримок перед циклом: лог займає UART на мілісекунди,
  // і задачі перестали б працювати одночасно.
  for (uint32_t i = 0; i < ITERATIONS; ++i) {
    s_tasks_done++;  // load -> add -> store: НЕ атомарно
  }

  // atomic_fetch_add(&s_tasks_done, 1);
  s_tasks_done++;
  vTaskDelete(NULL);  // задача FreeRTOS не може просто повернутися з функції
}

void app_main(void) {
  ESP_LOGI(TAG, "=== Гонка: 2 задачі x %" PRIu32 " інкрементів, ядра %d і %d ===",
           ITERATIONS, TASK_A_CORE, TASK_B_CORE);

  // app_main має пріоритет 1, а робітники - 5. Без цього перша ж задача на ядрі 0
  // витіснила б app_main, і другу задачу ми створили б, коли перша вже все порахувала.
  // Тому на час створення app_main стає важливішою за робітників, а потім повертається.
  const UBaseType_t main_prio = uxTaskPriorityGet(NULL);
  vTaskPrioritySet(NULL, PRIO_INIT);

  for (size_t i = 0; i < TASK_COUNT; ++i) {
    check_task_created(xTaskCreatePinnedToCore(increment_task, TASKS[i].name,
                                               LOG_TASK_STACK_BYTES, NULL,
                                               PRIO_WORKER, NULL, TASKS[i].core),
                       TASKS[i].name);
  }

  // Старт: задача на ядрі 0 чекала, поки app_main поверне пріоритет. Задача на ядрі 1
  // стартувала одразу при створенні (app_main займає лише ядро 0) - на десятки мкс
  // раніше, що на тлі десятків мс роботи нічого не змінює.
  vTaskPrioritySet(NULL, main_prio);

  // Чекаємо фінішу. vTaskDelay між перевірками - не busy-wait: процесор вільний.
  // Таймаут: якщо задача зависла, побачимо це в лозі, а не вічну тишу.
  TickType_t waited = 0;
  while (atomic_load(&s_tasks_done) < TASK_COUNT) {
    if (waited >= DONE_TIMEOUT) {
      ESP_LOGE(TAG, "Задачі не закінчили за %" PRIu32 " мс", (uint32_t)pdTICKS_TO_MS(waited));
      return;
    }
    vTaskDelay(POLL_PERIOD);
    waited += POLL_PERIOD;
  }

  const uint32_t expected = 2 * ITERATIONS;
  const uint32_t actual = s_tasks_done;
  if (actual == expected) {
    ESP_LOGI(TAG, "%" PRIu32 " з %" PRIu32 " - цього разу пощастило, натисни RST", actual,
             expected);
  } else {
    ESP_LOGE(TAG, "%" PRIu32 " з %" PRIu32 " - втрачено %" PRIu32 " інкрементів (%" PRIu32 "%%)",
             actual, expected, expected - actual,
             (expected - actual) * UINT32_C(100) / expected);
  }

  ESP_LOGI(TAG, "Висновок: спільна змінна без захисту = недетермінований результат. "
                "Як прибрати гонку - крок 2 (черга).");
}
