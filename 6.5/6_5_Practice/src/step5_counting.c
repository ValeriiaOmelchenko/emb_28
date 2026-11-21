// Крок 5: лічильний семафор - пул з 2 буферів на 4 задачі.
//
// Є POOL_SIZE = 2 статичні буфери (наприклад, під пакет вимірювань) і 4 задачі, яким вони
// потрібні. Одночасно працювати можуть лише дві. Лічильний семафор - це табло вільних місць
// на парковці: Take - заїхав (лічильник - 1), Give - виїхав (+ 1). На нулі наступний чекає.
//
// xSemaphoreCreateCounting(max, initial):
//   (2, 2) - пул: 2 місця, на старті всі вільні (цей крок);
//   (N, 0) - лічильник подій: на старті нічого не сталося, кожна подія - Give
//            (так у кроках 1-2 app_main чекав, поки обидві задачі закінчать раунд).
//
// Важливо: семафор каже лише СКІЛЬКИ місць вільно, а не ЯКЕ. Вибрати конкретний буфер -
// це читання і запис спільної таблиці зайнятості, тобто знову Read-Modify-Write.
// Тому таблицю захищає окремий mutex. Лічильний семафор не захищає дані - він обмежує
// кількість учасників.

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "practice.h"

#define POOL_SIZE         UINT32_C(2)
#define USER_COUNT        UINT32_C(4)
#define BUFFER_BYTES      UINT32_C(64)
#define SLOT_WAIT_MS      UINT32_C(400)  // скільки задача готова чекати вільний буфер
#define TABLE_TIMEOUT_MS  UINT32_C(50)   // таблицю тримають мікросекунди; 50 мс - явна аварія
#define WORK_MS           UINT32_C(300)  // скільки задача працює з буфером
#define PRIO_USER         ((UBaseType_t)5)

#define NO_SLOT UINT32_MAX

static const char *TAG = "pool";

// Пул: самі буфери і таблиця зайнятості.
static uint8_t s_buffers[POOL_SIZE][BUFFER_BYTES];
static bool s_slot_busy[POOL_SIZE];

static SemaphoreHandle_t s_pool_sem;    // скільки буферів вільно
static SemaphoreHandle_t s_table_mutex; // хто з них вільний
static StaticSemaphore_t s_pool_sem_buf;
static StaticSemaphore_t s_table_mutex_buf;

// Пауза між спробами різна для кожної задачі, щоб вони не стукали синхронно.
static const uint32_t USER_PAUSE_MS[USER_COUNT] = {100, 150, 200, 250};
static_assert(sizeof(USER_PAUSE_MS) / sizeof(USER_PAUSE_MS[0]) == USER_COUNT, "пауза для кожної задачі");

// Знаходить і позначає вільний буфер. Викликати лише ПІСЛЯ успішного Take семафора пулу:
// тоді вільний буфер гарантовано є.
static uint32_t slot_acquire(void) {
  uint32_t slot = NO_SLOT;
  if (xSemaphoreTake(s_table_mutex, pdMS_TO_TICKS(TABLE_TIMEOUT_MS)) == pdTRUE) {
    for (uint32_t i = 0; i < POOL_SIZE; i++) {
      if (!s_slot_busy[i]) {
        s_slot_busy[i] = true;
        slot = i;
        break;
      }
    }
    xSemaphoreGive(s_table_mutex);
  }
  return slot;
}

// Повертає false, якщо не вдалося взяти таблицю: тоді слот лишається позначеним, і
// семафор НЕ можна віддавати, інакше лічильник розійдеться з таблицею.
static bool slot_release(uint32_t slot) {
  if (xSemaphoreTake(s_table_mutex, pdMS_TO_TICKS(TABLE_TIMEOUT_MS)) != pdTRUE) {
    return false;
  }
  s_slot_busy[slot] = false;
  xSemaphoreGive(s_table_mutex);
  return true;
}

static void user_task(void *arg) {
  const uint32_t id = (uint32_t)(uintptr_t)arg;
  uint32_t done = 0;
  uint32_t gave_up = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(USER_PAUSE_MS[id]));

    // 1. Місце на парковці. Не дочекались - не висимо вічно, а фіксуємо відмову.
    if (xSemaphoreTake(s_pool_sem, pdMS_TO_TICKS(SLOT_WAIT_MS)) != pdTRUE) {
      gave_up++;
      ESP_LOGW(TAG, "user%" PRIu32 ": усі буфери зайняті понад %" PRIu32 " мс, пропускаю (відмов: %" PRIu32 ")",
               id, SLOT_WAIT_MS, gave_up);
      continue;
    }

    // 2. Конкретний буфер.
    const uint32_t slot = slot_acquire();
    if (slot == NO_SLOT) {
      ESP_LOGE(TAG, "user%" PRIu32 ": семафор дав місце, а вільного буфера немає", id);
      xSemaphoreGive(s_pool_sem);
      continue;
    }

    // 3. Робота з буфером. Mutex тут не потрібен: цей буфер зараз належить лише нам.
    const UBaseType_t free_now = uxSemaphoreGetCount(s_pool_sem);
    ESP_LOGI(TAG, "user%" PRIu32 " -> буфер %" PRIu32 ", вільно ще: %u", id, slot, (unsigned)free_now);
    snprintf((char *)s_buffers[slot], BUFFER_BYTES, "user%" PRIu32 " #%" PRIu32, id, done);
    vTaskDelay(pdMS_TO_TICKS(WORK_MS)); // «заповнюємо і обробляємо пакет»
    done++;

    // 4. Повертаємо у зворотному порядку: спершу буфер, потім місце.
    if (slot_release(slot)) {
      xSemaphoreGive(s_pool_sem);
    } else {
      ESP_LOGE(TAG, "user%" PRIu32 ": не вдалось звільнити буфер %" PRIu32, id, slot);
    }
  }
}

void app_main(void) {
  // Пул: максимум POOL_SIZE, на старті вільні всі POOL_SIZE.
  s_pool_sem = xSemaphoreCreateCountingStatic(POOL_SIZE, POOL_SIZE, &s_pool_sem_buf);
  s_table_mutex = xSemaphoreCreateMutexStatic(&s_table_mutex_buf);

  // Задачі на обох ядрах: пулу байдуже, звідки прийшов запит.
  for (uint32_t i = 0; i < USER_COUNT; i++) {
    char name[configMAX_TASK_NAME_LEN];
    snprintf(name, sizeof(name), "user%" PRIu32, i);
    check_task_created(xTaskCreatePinnedToCore(user_task, name, TASK_STACK_BYTES,
                                               (void *)(uintptr_t)i, PRIO_USER, NULL,
                                               (i % 2U == 0U) ? CORE_0 : CORE_1),
                       name);
  }
  ESP_LOGI(TAG, "%" PRIu32 " задачі, %" PRIu32 " буфери", USER_COUNT, POOL_SIZE);
}
