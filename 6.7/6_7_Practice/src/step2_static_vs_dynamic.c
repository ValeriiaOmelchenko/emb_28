// Крок 2: xTaskCreate() проти xTaskCreateStatic(). Звідки береться памʼять задачі
// і що буде, коли heap закінчився.
// Нічого підключати не треба.
//
// Сценарій:
//   1) xTaskCreate()       - стек і TCB беруться з heap: free падає на ~стек + TCB;
//   2) xTaskCreateStatic() - стек і TCB у наших static-масивах (.bss): free не змінюється;
//   3) «баласт» зʼїдає heap (наче Wi-Fi і драйвери вже все забрали);
//   4) ще раз обидва способи: динамічна задача - errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY,
//      статична створюється, бо памʼять під неї зарезервував лінкер ще при збірці.
//
// HUGE_STATIC_STACK - змінити на 1 і зібрати (навіть не прошивати):
//   статичний стек 400 КБ не влазить у DRAM, і помилка буде НА ЕТАПІ ЛІНКУВАННЯ
//   («region `dram0_0_seg' overflowed»), а не в runtime на обʼєкті через місяць.

#include <assert.h>
#include <inttypes.h>
#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "practice.h"

#define HUGE_STATIC_STACK 0

#define DEMO_STACK_BYTES  UINT32_C(4096)
#if HUGE_STATIC_STACK
#define LATE_STACK_BYTES  (UINT32_C(400) * BYTES_PER_KB) // більше, ніж уся внутрішня SRAM
#else
#define LATE_STACK_BYTES  DEMO_STACK_BYTES
#endif
#define BALLAST_LEAVE_B   UINT32_C(2048) // лишаємо в heap менше, ніж треба під стек задачі
#define ALIVE_PERIOD_MS   UINT32_C(3000)
#define SETTLE_MS         UINT32_C(100)  // Idle Task звільняє памʼять видалених задач - даємо їй час
#define PRIO_DEMO         ((UBaseType_t)2)

// В ESP-IDF StackType_t = uint8_t, тому глибина стеку - у байтах і масив байтовий.
// У ванільному FreeRTOS (STM32) StackType_t = uint32_t і глибина - у словах.
static_assert(sizeof(StackType_t) == 1, "ESP-IDF: стек задається в байтах");

static const char *TAG = "static";

// Памʼять статичних задач: виділена лінкером, у heap її немає взагалі.
static StackType_t s_early_stack[DEMO_STACK_BYTES];
static StaticTask_t s_early_tcb;
static StackType_t s_late_stack[LATE_STACK_BYTES];
static StaticTask_t s_late_tcb;

static heap_ballast_t s_ballast;

static void demo_task(void *arg) {
  const char *who = arg;
  for (;;) {
    ESP_LOGI(TAG, "%s: жива", who);
    vTaskDelay(pdMS_TO_TICKS(ALIVE_PERIOD_MS));
  }
}

void app_main(void) {
  ESP_LOGI(TAG, "TCB (StaticTask_t) = %zu B, стек = %" PRIu32 " B", sizeof(StaticTask_t), DEMO_STACK_BYTES);

  // --- 1) Динамічна задача: памʼять з heap ---
  log_heap(TAG, "старт");
  TaskHandle_t dyn = NULL;
  BaseType_t res = xTaskCreate(demo_task, "dyn", DEMO_STACK_BYTES, "dyn (heap)", PRIO_DEMO, &dyn);
  check_task_created(res, "dyn");
  log_heap(TAG, "після xTaskCreate");

  // --- 2) Статична задача: памʼять у .bss ---
  TaskHandle_t early = xTaskCreateStatic(demo_task, "static1", DEMO_STACK_BYTES, "static1 (.bss)", PRIO_DEMO,
                                         s_early_stack, &s_early_tcb);
  // xTaskCreateStatic() повертає NULL лише якщо передали NULL-буфери, памʼяті «не вистачити» не може.
  configASSERT(early != NULL);
  log_heap(TAG, "після xTaskCreateStatic");

  // Динамічну задачу прибираємо: її стек і TCB повернуться в heap (звільняє Idle Task).
  vTaskDelete(dyn);
  vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));

  // --- 3) Штучна нестача памʼяті ---
  heap_ballast_take(&s_ballast, BALLAST_LEAVE_B);
  log_heap(TAG, "баласт зʼїв heap");

  // --- 4) Повторна спроба обома способами ---
  res = xTaskCreate(demo_task, "dyn2", DEMO_STACK_BYTES, "dyn2 (heap)", PRIO_DEMO, NULL);
  if (res != pdPASS) {
    // Не abort(), а лог: показуємо, що результат ТРЕБА перевіряти. Без перевірки задача
    // просто «не працює», і ніхто не знає чому.
    ESP_LOGE(TAG, "xTaskCreate: помилка %d (errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY = %d)", (int)res,
             (int)errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY);
  }

  TaskHandle_t late = xTaskCreateStatic(demo_task, "static2", LATE_STACK_BYTES, "static2 (.bss, heap порожній)",
                                        PRIO_DEMO, s_late_stack, &s_late_tcb);
  configASSERT(late != NULL);
  ESP_LOGI(TAG, "xTaskCreateStatic при порожньому heap: створено");

  vTaskDelay(pdMS_TO_TICKS(ALIVE_PERIOD_MS * UINT32_C(2)));
  heap_ballast_release(&s_ballast);
  log_heap(TAG, "баласт віддано");
  // Далі в лозі живуть static1 і static2. dyn2 так і не зʼявився.
}
