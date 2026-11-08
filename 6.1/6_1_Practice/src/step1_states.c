// Крок 1 (підтема 2): перша задача FreeRTOS і стани задачі.
//
// Беремо один процес з 2.3 - червоний LED, 200 мс - і робимо з нього задачу.
// Поруч ще дві задачі, щоб у Serial Monitor було видно всі чотири стани:
//   red     - блимає і майже весь час спить у vTaskDelay()      → Blocked;
//   worker  - «важка» фонова робота на ядрі 1 з низьким пріоритетом;
//             коли monitor (вищий пріоритет, те саме ядро) її витісняє → Ready;
//   monitor - друкує стани раз на 500 мс; про себе завжди бачить     → Running.
// Раз на 10 с monitor на 3 с призупиняє red (vTaskSuspend)          → Suspended,
// і LED завмирає в тому стані, в якому його застали.

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "practice.h"

#define MONITOR_PERIOD_MS UINT32_C(500)
#define WORKER_BUSY_US    UINT32_C(8000)    // 8 мс «рахує», не віддаючи процесор
#define WORKER_SLEEP_MS   UINT32_C(2)       // 2 мс спить: IDLE на ядрі 1 теж має жити
#define CYCLE_MS          UINT32_C(10000)
#define SUSPEND_FROM_MS   UINT32_C(3000)    // у кожному 10-секундному циклі red
#define SUSPEND_TO_MS     UINT32_C(6000)    // призупинено з 3-ї по 6-ту секунду
#define DEMO_CORE         ((BaseType_t)1)   // worker і monitor ділять одне ядро
#define PRIO_WORKER       ((UBaseType_t)1)
#define PRIO_MONITOR      ((UBaseType_t)3)  // вище за worker → витісняє його

// static_assert (C11, <assert.h>): перевірка констант під час компіляції.
static_assert(SUSPEND_FROM_MS < SUSPEND_TO_MS && SUSPEND_TO_MS <= CYCLE_MS,
              "вікно Suspended має лежати всередині циклу");

// static на рівні файлу = видно лише в цьому .c (аналог анонімного namespace в C++).
static TaskHandle_t s_red    = NULL;
static TaskHandle_t s_worker = NULL;

static const char *state_name(eTaskState state) {
  switch (state) {
    case eRunning:   return "Running";
    case eReady:     return "Ready";
    case eBlocked:   return "Blocked";
    case eSuspended: return "Suspended";
    case eDeleted:   return "Deleted";
    default:         return "Invalid";
  }
}

// Перша задача: той самий процес, що в 2.3, але замість «чи настав час?» у суперлупі
// задача просто засинає. Поки вона Blocked, планувальник віддає процесор іншим.
static void red_task(void *arg) {
  (void)arg;  // параметр не потрібен: (void) прибирає попередження «unused parameter»
  bool on = false;
  for (;;) {
    on = !on;
    gpio_set_level(LED_RED, on ? 1 : 0);
    vTaskDelay(pdMS_TO_TICKS(RED_TOGGLE_MS));
  }
}

// Фонова робота, яка майже не спить. esp_rom_delay_us() - активне очікування:
// задача весь цей час хоче процесор. Саме тому, коли її витісняє monitor, вона Ready.
static void worker_task(void *arg) {
  (void)arg;
  for (;;) {
    esp_rom_delay_us(WORKER_BUSY_US);
    vTaskDelay(pdMS_TO_TICKS(WORKER_SLEEP_MS));
  }
}

static void monitor_task(void *arg) {
  (void)arg;
  const TaskHandle_t self = xTaskGetCurrentTaskHandle();
  TickType_t last_wake = xTaskGetTickCount();

  for (;;) {
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t phase = now_ms % CYCLE_MS;
    const bool should_suspend = phase >= SUSPEND_FROM_MS && phase < SUSPEND_TO_MS;
    const bool is_suspended   = eTaskGetState(s_red) == eSuspended;

    if (should_suspend && !is_suspended) {
      vTaskSuspend(s_red);
      printf(">>> vTaskSuspend(red): LED завмирає\n");
    } else if (!should_suspend && is_suspended) {
      vTaskResume(s_red);
      printf(">>> vTaskResume(red): LED блимає далі\n");
    }

    printf("t=%6" PRIu32 " ms | red: %-9s | worker: %-9s | monitor: %s\n",
           now_ms,
           state_name(eTaskGetState(s_red)),
           state_name(eTaskGetState(s_worker)),
           state_name(eTaskGetState(self)));

    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(MONITOR_PERIOD_MS));
  }
}

// У C немає name mangling, тому extern "C" не потрібен: ESP-IDF знайде app_main за іменем.
void app_main(void) {
  printf("Practice 6.1 step 1: task states\n");
  led_init(LED_RED);

  // Без привʼязки до ядра: на двоядерному S3 SMP-планувальник сам вибере вільне ядро.
  check_task_created(xTaskCreate(red_task, "red", LED_STACK_BYTES, NULL, PRIO_LED, &s_red));

  // worker і monitor навмисно на одному ядрі: інакше вони б не конкурували,
  // і стан Ready побачити було б важко.
  check_task_created(xTaskCreatePinnedToCore(worker_task, "worker", LED_STACK_BYTES,
                                             NULL, PRIO_WORKER, &s_worker, DEMO_CORE));
  check_task_created(xTaskCreatePinnedToCore(monitor_task, "monitor", PRINT_STACK_BYTES,
                                             NULL, PRIO_MONITOR, NULL, DEMO_CORE));

  // app_main() - теж задача. Вона завершується, а створені задачі живуть далі.
}
