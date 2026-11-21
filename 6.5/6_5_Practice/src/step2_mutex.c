// Крок 2: той самий лічильник, що в кроці 1, але під mutex.
//
// Раунди по черзі в трьох режимах, щоб порівняти результат і час:
//   UNPROTECTED - як у кроці 1: швидко і неправильно.
//   MUTEX_SHORT - mutex навколо одного counter++: правильно, критична секція мінімальна.
//   MUTEX_LONG  - mutex навколо всього циклу: правильно, але задачі працюють по черзі,
//                 і друга задача весь цикл першої стоїть у Blocked.
//
// Mutex (Mutual Exclusion) - ключ від кімнати з ресурсом. Ключ один, і його ВЛАСНИК (задача, що
// взяла ключ) єдиний, хто може його повернути. FreeRTOS памʼятає власника (pxMutexHolder) -
// на цьому побудоване успадкування пріоритету (крок 6).
//
// Правила, які показує цей крок:
//   1. Кожен доступ до спільних даних - під тим самим mutex. Один незахищений доступ ламає все.
//   2. Take з таймаутом і перевірка результату. Не вдалось - лог і рішення, а не вічне зависання.
//   3. Критична секція (між Take і Give) - якомога коротша: поки ключ у тебе, інші чекають.
//   4. Mutex не безкоштовний: Take/Give - це виклики ядра FreeRTOS. Для одного лічильника
//      промисловий вибір - атомарна операція (див. README, «На виріст»). Mutex - для ресурсів,
//      з якими працюють довше за одну інструкцію: буфер, структура, драйвер.

#include <assert.h>
#include <inttypes.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "practice.h"

// Менше, ніж у кроці 1: 200 000 пар Take/Give займають сотні мілісекунд.
#define INCREMENTS_PER_TASK UINT32_C(100000)
#define WORKER_COUNT        UINT32_C(2)
#define MUTEX_TIMEOUT_MS    UINT32_C(100)
#define ROUND_PAUSE_MS      UINT32_C(2000)
#define ROUND_TIMEOUT_MS    UINT32_C(5000)
#define PRIO_WORKER         ((UBaseType_t)5)

static const char *TAG = "mutex";

typedef enum {
  MODE_UNPROTECTED,
  MODE_MUTEX_SHORT,
  MODE_MUTEX_LONG,
  MODE_COUNT,
} inc_mode_t;

static const char *const MODE_NAMES[] = {
    [MODE_UNPROTECTED] = "без захисту ",
    [MODE_MUTEX_SHORT] = "mutex на ++ ",
    [MODE_MUTEX_LONG] = "mutex на цикл",
};
static_assert(sizeof(MODE_NAMES) / sizeof(MODE_NAMES[0]) == MODE_COUNT, "імʼя для кожного режиму");

static volatile uint32_t s_counter;

static SemaphoreHandle_t s_counter_mutex;
static StaticSemaphore_t s_counter_mutex_buf;

// Режим раунду. Пише лише app_main, і лише поки задачі сплять на s_start.
// Семафор старту гарантує, що задача прочитає вже нове значення.
static inc_mode_t s_mode;

// Скільки разів Take не встиг за таймаут - окрема клітинка на кожну задачу.
// У кожної клітинки один писач (своя задача), тому захист не потрібен.
static uint32_t s_timeouts[WORKER_COUNT];

static SemaphoreHandle_t s_start;
static SemaphoreHandle_t s_done;
static StaticSemaphore_t s_start_buf;
static StaticSemaphore_t s_done_buf;

static void increment_short_sections(uint32_t id) {
  for (uint32_t i = 0; i < INCREMENTS_PER_TASK; i++) {
    if (xSemaphoreTake(s_counter_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
      s_counter++; // критична секція - одна операція
      xSemaphoreGive(s_counter_mutex);
    } else {
      s_timeouts[id]++; // не вдалось - інкремент втрачено, але ми ЗНАЄМО про це
    }
  }
}

static void increment_long_section(uint32_t id) {
  if (xSemaphoreTake(s_counter_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
    s_timeouts[id]++;
    return;
  }
  for (uint32_t i = 0; i < INCREMENTS_PER_TASK; i++) {
    s_counter++;
  }
  xSemaphoreGive(s_counter_mutex);
}

static void worker_task(void *arg) {
  const uint32_t id = (uint32_t)(uintptr_t)arg; // номер задачі передано через параметр, не через глобальну змінну
  for (;;) {
    xSemaphoreTake(s_start, portMAX_DELAY); // чекаємо команду, а не ресурс - див. крок 1

    switch (s_mode) {
      case MODE_UNPROTECTED:
        for (uint32_t i = 0; i < INCREMENTS_PER_TASK; i++) {
          s_counter++;
        }
        break;
      case MODE_MUTEX_SHORT:
        increment_short_sections(id);
        break;
      case MODE_MUTEX_LONG:
        increment_long_section(id);
        break;
      case MODE_COUNT:
        break;
    }

    xSemaphoreGive(s_done);
  }
}

void app_main(void) {
  // Mutex створюємо ДО задач, які його беруть. Static-версія: памʼять - глобальна змінна, а не heap.
  s_counter_mutex = xSemaphoreCreateMutexStatic(&s_counter_mutex_buf);
  s_start = xSemaphoreCreateCountingStatic(WORKER_COUNT, 0, &s_start_buf);
  s_done = xSemaphoreCreateCountingStatic(WORKER_COUNT, 0, &s_done_buf);

  check_task_created(xTaskCreatePinnedToCore(worker_task, "worker0", TASK_STACK_BYTES,
                                             (void *)(uintptr_t)0, PRIO_WORKER, NULL, CORE_0),
                     "worker0");
  check_task_created(xTaskCreatePinnedToCore(worker_task, "worker1", TASK_STACK_BYTES,
                                             (void *)(uintptr_t)1, PRIO_WORKER, NULL, CORE_1),
                     "worker1");

  const uint32_t expected = WORKER_COUNT * INCREMENTS_PER_TASK;
  for (uint32_t round = 0;; round++) {
    s_mode = (inc_mode_t)(round % MODE_COUNT);
    s_counter = 0;
    for (uint32_t i = 0; i < WORKER_COUNT; i++) {
      s_timeouts[i] = 0;
    }

    const int64_t t_start_us = esp_timer_get_time();
    for (uint32_t i = 0; i < WORKER_COUNT; i++) {
      xSemaphoreGive(s_start);
    }
    for (uint32_t i = 0; i < WORKER_COUNT; i++) {
      if (xSemaphoreTake(s_done, pdMS_TO_TICKS(ROUND_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Задача не завершилась за %" PRIu32 " мс", ROUND_TIMEOUT_MS);
      }
    }
    const int64_t elapsed_ms = (esp_timer_get_time() - t_start_us) / 1000;

    const uint32_t got = s_counter;
    const uint32_t timeouts = s_timeouts[0] + s_timeouts[1];
    if (got == expected) {
      ESP_LOGI(TAG, "%s: %6" PRIu32 " з %" PRIu32 "  OK         %4" PRId64 " мс",
               MODE_NAMES[s_mode], got, expected, elapsed_ms);
    } else {
      ESP_LOGW(TAG, "%s: %6" PRIu32 " з %" PRIu32 "  -%-6" PRIu32 "    %4" PRId64 " мс, таймаутів: %" PRIu32,
               MODE_NAMES[s_mode], got, expected, expected - got, elapsed_ms, timeouts);
    }

    if (s_mode == MODE_COUNT - 1) {
      ESP_LOGI(TAG, "----");
    }
    vTaskDelay(pdMS_TO_TICKS(ROUND_PAUSE_MS));
  }
}
