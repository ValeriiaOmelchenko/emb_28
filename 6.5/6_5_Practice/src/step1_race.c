// Крок 1: Race Condition. Дві задачі на РІЗНИХ ядрах інкрементують один лічильник.
//
// Кожна задача робить counter++ рівно INCREMENTS_PER_TASK разів. Очікуємо 2 * INCREMENTS_PER_TASK.
// Отримуємо менше, і щоразу по-різному.
//
// Чому: counter++ - це не одна операція, а три інструкції процесора (Read-Modify-Write):
//     l32i  a8, counter   ; READ   - прочитати з памʼяті в регістр
//     addi  a8, a8, 1     ; MODIFY - додати 1 у регістрі
//     s32i  a8, counter   ; WRITE  - записати назад
// Ядро 0 і ядро 1 одночасно прочитали 5, обидва додали 1, обидва записали 6. Один інкремент зник.
// Аналогія - банкомат: двоє одночасно бачать на рахунку $100 і обидва знімають по $80.
//
// На ESP32-S3 це ФІЗИЧНО одночасно - два ядра працюють паралельно. На одноядерному
// STM32F411 гонка теж є, але лише коли планувальник витіснить задачу точно між READ і WRITE.
// Це можна побачити тут: SAME_CORE 1 садить обидві задачі на одне ядро.
//
// volatile тут НЕ рятує. Він лише забороняє компілятору тримати counter у регістрі
// (кожен ++ справді йде в памʼять), але не робить READ-MODIFY-WRITE неподільним.
// Без volatile компілятор взагалі злив би цикл в одне counter += 1000000, і гонку не було б видно.

#include <inttypes.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "practice.h"

// 0 - задачі на різних ядрах (апаратний паралелізм, ESP32-S3)
// 1 - обидві на ядрі 0 (як на одноядерному МК: гонка лише через витіснення, втрат значно менше)
#define SAME_CORE 0

#define INCREMENTS_PER_TASK UINT32_C(1000000)
#define WORKER_COUNT        UINT32_C(2)
#define ROUND_PAUSE_MS      UINT32_C(3000)
#define ROUND_TIMEOUT_MS    UINT32_C(5000) // раунд триває десятки мс; 5 с - явна ознака, що щось зависло
#define PRIO_WORKER         ((UBaseType_t)5)

static const char *TAG = "race";

// Спільний ресурс, за який битимуться два ядра.
static volatile uint32_t s_counter;

// Старт і фініш раунду. Це семафори - тема кроків 4-5. Тут вони просто «стартовий пістолет»
// (app_main дає 2 сигнали, кожна задача забирає один) і «фінішна лінія» (кожна задача дає 1).
static SemaphoreHandle_t s_start;
static SemaphoreHandle_t s_done;
static StaticSemaphore_t s_start_buf;
static StaticSemaphore_t s_done_buf;

static void worker_task(void *arg) {
  (void)arg;
  for (;;) {
    // Чекати команду старту вічно - нормально: задача спить у Blocked і не їсть CPU.
    // portMAX_DELAY небезпечний там, де чекаємо РЕСУРС, який хтось може не віддати (крок 3).
    xSemaphoreTake(s_start, portMAX_DELAY);

    for (uint32_t i = 0; i < INCREMENTS_PER_TASK; i++) {
      s_counter++; // READ - MODIFY - WRITE, без жодного захисту
    }

    xSemaphoreGive(s_done);
  }
}

void app_main(void) {
  s_start = xSemaphoreCreateCountingStatic(WORKER_COUNT, 0, &s_start_buf);
  s_done = xSemaphoreCreateCountingStatic(WORKER_COUNT, 0, &s_done_buf);

  const BaseType_t second_core = SAME_CORE ? CORE_0 : CORE_1;
  check_task_created(xTaskCreatePinnedToCore(worker_task, "worker0", TASK_STACK_BYTES, NULL,
                                             PRIO_WORKER, NULL, CORE_0),
                     "worker0");
  check_task_created(xTaskCreatePinnedToCore(worker_task, "worker1", TASK_STACK_BYTES, NULL,
                                             PRIO_WORKER, NULL, second_core),
                     "worker1");

  ESP_LOGI(TAG, "Задачі на ядрах 0 і %d, кожна робить %" PRIu32 " інкрементів",
           (int)second_core, INCREMENTS_PER_TASK);

  const uint32_t expected = WORKER_COUNT * INCREMENTS_PER_TASK;
  for (uint32_t round = 1;; round++) {
    s_counter = 0; // задачі зараз сплять на s_start, тому тут ніхто не заважає

    const int64_t t_start_us = esp_timer_get_time();
    for (uint32_t i = 0; i < WORKER_COUNT; i++) {
      xSemaphoreGive(s_start);
    }
    for (uint32_t i = 0; i < WORKER_COUNT; i++) {
      if (xSemaphoreTake(s_done, pdMS_TO_TICKS(ROUND_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Раунд %" PRIu32 ": задача не завершилась за %" PRIu32 " мс", round,
                 ROUND_TIMEOUT_MS);
      }
    }
    const int64_t elapsed_ms = (esp_timer_get_time() - t_start_us) / 1000;

    const uint32_t got = s_counter;
    const uint32_t lost = expected - got;
    if (lost == 0) {
      ESP_LOGI(TAG, "Раунд %" PRIu32 ": %" PRIu32 " з %" PRIu32 " - пощастило (%" PRId64 " мс)",
               round, got, expected, elapsed_ms);
    } else {
      ESP_LOGW(TAG,
               "Раунд %" PRIu32 ": %" PRIu32 " з %" PRIu32 ", загублено %" PRIu32
               " (%" PRIu32 ".%" PRIu32 "%%), %" PRId64 " мс",
               round, got, expected, lost, lost * 100U / expected, (lost * 1000U / expected) % 10U,
               elapsed_ms);
    }

    vTaskDelay(pdMS_TO_TICKS(ROUND_PAUSE_MS));
  }
}
