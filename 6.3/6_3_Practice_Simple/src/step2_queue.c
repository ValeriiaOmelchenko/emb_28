// Крок 2: та сама задача з кроку 1, але без гонки - через чергу (xQueue).
//
// У лічильника ОДИН власник - задача consumer. Продюсери на двох ядрах його не чіпають,
// а лише кладуть у чергу «+1 від мене». Черга копіює дані і сама захищена всередині
// FreeRTOS, тому безпечна між задачами і ядрами.
// Аналогія: касири не пишуть у зошит, а кидають чеки в скриньку,
// а бухгалтер (consumer) по одному виймає і записує.

#include <inttypes.h>
#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "practice.h"

#define ITERATIONS     UINT32_C(10000)          // черга повільніша за ++, тож менше ітерацій
#define QUEUE_LENGTH   ((UBaseType_t)16)
#define SEND_TIMEOUT   pdMS_TO_TICKS(100)       // скільки продюсер чекає місця в черзі
#define IDLE_TIMEOUT   pdMS_TO_TICKS(1000)      // тиша довше - продюсери закінчили
#define PRIO_PRODUCER  ((UBaseType_t)1)
#define PRIO_CONSUMER  ((UBaseType_t)2)         // вище за продюсерів: черга не переповнюється

static const char *TAG = "step2_queue";

static QueueHandle_t s_queue;  // створюється ДО задач, що нею користуються

// Повідомлення - номер продюсера (0 або 1). Черга копіює 1 байт, а не вказівник.
static void producer_task(void *arg) {
  const uint8_t id = (uint8_t)(uintptr_t)arg;
  uint32_t dropped = 0;
  for (uint32_t i = 0; i < ITERATIONS; ++i) {
    // Обмежений таймаут замість portMAX_DELAY: якщо consumer завис,
    // продюсер не застрягне назавжди мовчки, а порахує втрати.
    if (xQueueSend(s_queue, &id, SEND_TIMEOUT) != pdPASS) {
      ++dropped;
    }
  }
  ESP_LOGI(TAG, "Продюсер %u (ядро %d): втрачено %" PRIu32, (unsigned)id, (int)xPortGetCoreID(),
           dropped);
  vTaskDelete(NULL);
}

static void consumer_task(void *arg) {
  (void)arg;
  // Локальні змінні: їх бачить лише ця задача - гонки немає за побудовою.
  uint32_t counter = 0;
  uint32_t per_producer[2] = {0};

  for (;;) {
    uint8_t id;
    // Таймаут, а не portMAX_DELAY: тиша = продюсери закінчили, час підбити підсумок.
    if (xQueueReceive(s_queue, &id, IDLE_TIMEOUT) != pdPASS) {
      ESP_LOGI(TAG, "Лічильник %" PRIu32 " з %" PRIu32 " (A: %" PRIu32 ", B: %" PRIu32 ")", counter,
               2 * ITERATIONS, per_producer[0], per_producer[1]);
      continue;
    }
    if (id < 2) {
      ++per_producer[id];
      ++counter;
    }
  }
}

void app_main(void) {
  ESP_LOGI(TAG, "=== Черга: 2 продюсери x %" PRIu32 " повідомлень ===", ITERATIONS);

  // 1. Черга - першою: задача, що стартує раніше за xQueueCreate, отримала б NULL.
  //    Heap один раз на старті - нормально; погано - створювати черги в циклі.
  s_queue = xQueueCreate(QUEUE_LENGTH, sizeof(uint8_t));
  if (s_queue == NULL) {
    ESP_LOGE(TAG, "Не вдалося створити чергу");
    abort();
  }

  // 2. Consumer, потім продюсери - по одному на кожне ядро, як у кроці 1.
  check_task_created(
      xTaskCreate(consumer_task, "consumer", TASK_STACK_BYTES, NULL, PRIO_CONSUMER, NULL),
      "consumer");
  check_task_created(xTaskCreatePinnedToCore(producer_task, "prod_A", TASK_STACK_BYTES, (void *)0,
                                             PRIO_PRODUCER, NULL, 0),
                     "prod_A");
  check_task_created(xTaskCreatePinnedToCore(producer_task, "prod_B", TASK_STACK_BYTES, (void *)1,
                                             PRIO_PRODUCER, NULL, 1),
                     "prod_B");
}
