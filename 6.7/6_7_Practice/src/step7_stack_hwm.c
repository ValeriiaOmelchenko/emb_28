// Крок 7 (доповнено, в плані уроку його немає окремим кроком): підбір стеку за High Water Mark
// і переповнення стеку. Нічого підключати не треба.
//
// Три задачі з ОДНАКОВИМ стеком 3 КБ, різна лише робота:
//   quiet  - рахує, нічого не друкує;
//   logger - друкує ESP_LOGI з форматуванням (printf-сімейство - ненажера стеку);
//   buffer - кладе на стек локальний масив LOCAL_BUF_B.
// app_main раз на 2 с друкує HWM кожної задачі у байтах і відсотках з вердиктом.
//
// STACK_OVERFLOW - змінити на 1, прошити:
//   локальний масив більший за весь стек задачі. Перевірка «canary» (CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY,
//   увімкнена за замовчуванням) знаходить зіпсовані байти в кінці стеку при перемиканні задач
//   і викликає vApplicationStackOverflowHook(). В ESP-IDF він оголошений weak, тому можна
//   підставити свій - нижче.

#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "practice.h"

#define STACK_OVERFLOW 0

#define TASK_STACK_BYTES  UINT32_C(3072)
#if STACK_OVERFLOW
#define LOCAL_BUF_B       TASK_STACK_BYTES // масив не влазить у стек навіть сам по собі
#else
#define LOCAL_BUF_B       UINT32_C(512)
#endif
#define WORK_PERIOD_MS    UINT32_C(200)
#define REPORT_PERIOD_MS  UINT32_C(2000)
#define HWM_OK_PCT        UINT32_C(20)
#define HWM_WARN_PCT      UINT32_C(10)
#define PRIO_WORKER       ((UBaseType_t)2)
#define TASKS_COUNT       UINT32_C(3)

static const char *TAG = "stack";

typedef struct {
  const char *name;
  TaskFunction_t fn;
  TaskHandle_t handle;
} worker_t;

// Свій хук переповнення. Викликається з перемикача контексту: стек уже зіпсований,
// тому мінімум роботи - ROM-друк без heap і без логера, потім аварійний перезапуск.
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
  (void)xTask;
  esp_rom_printf("\n*** STACK OVERFLOW у задачі \"%s\": збільшити стек або прибрати великі локальні масиви ***\n",
                 pcTaskName);
  esp_system_abort("stack overflow");
}

static void quiet_task(void *arg) {
  (void)arg;
  volatile uint32_t counter = 0; // volatile - щоб компілятор не викинув «марну» роботу
  for (;;) {
    counter++;
    vTaskDelay(pdMS_TO_TICKS(WORK_PERIOD_MS));
  }
}

static void logger_task(void *arg) {
  (void)arg;
  for (uint32_t n = 1;; n++) {
    ESP_LOGI(TAG, "logger: n=%" PRIu32 " n/7=%.3f", n, (double)n / 7.0);
    vTaskDelay(pdMS_TO_TICKS(REPORT_PERIOD_MS));
  }
}

// noinline: масив лягає на стек саме цієї функції, а не розчиняється в задачі.
static __attribute__((noinline)) uint32_t use_local_buffer(uint32_t seed) {
  volatile uint8_t buf[LOCAL_BUF_B]; // volatile - кожен байт справді записується на стек
  for (uint32_t i = 0; i < LOCAL_BUF_B; i++) {
    buf[i] = (uint8_t)(seed + i);
  }
  return buf[LOCAL_BUF_B - 1U];
}

static void buffer_task(void *arg) {
  (void)arg;
  for (uint32_t seed = 0;; seed++) {
    (void)use_local_buffer(seed);
    vTaskDelay(pdMS_TO_TICKS(WORK_PERIOD_MS));
  }
}

static worker_t s_workers[TASKS_COUNT] = {
    {.name = "quiet", .fn = quiet_task},
    {.name = "logger", .fn = logger_task},
    {.name = "buffer", .fn = buffer_task},
};

static const char *verdict(uint32_t pct) {
  if (pct == 0U) {
    return "ПЕРЕПОВНЕННЯ";
  }
  if (pct < HWM_WARN_PCT) {
    return "мало: збільшити стек";
  }
  if (pct < HWM_OK_PCT) {
    return "впритул";
  }
  return "ок";
}

void app_main(void) {
  ESP_LOGI(TAG, "стек кожної задачі %" PRIu32 " B, локальний масив buffer %" PRIu32 " B", TASK_STACK_BYTES,
           LOCAL_BUF_B);
  for (uint32_t i = 0; i < TASKS_COUNT; i++) {
    check_task_created(xTaskCreate(s_workers[i].fn, s_workers[i].name, TASK_STACK_BYTES, NULL, PRIO_WORKER,
                                   &s_workers[i].handle),
                       s_workers[i].name);
  }

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(REPORT_PERIOD_MS));
    for (uint32_t i = 0; i < TASKS_COUNT; i++) {
      // HWM - мінімум вільного стеку за ВЕСЬ час життя задачі (стек заповнено 0xA5 при
      // створенні, рахуємо незаймані байти). Пік, який не настав під час тесту, він не бачить.
      const UBaseType_t hwm = uxTaskGetStackHighWaterMark(s_workers[i].handle);
      const uint32_t pct = stack_free_pct(hwm, TASK_STACK_BYTES);
      ESP_LOGI(TAG, "%-6s використано %4" PRIu32 " B, HWM %4u B (%2" PRIu32 "%%) - %s", s_workers[i].name,
               TASK_STACK_BYTES - (uint32_t)hwm, (unsigned)hwm, pct, verdict(pct));
    }
  }
}
