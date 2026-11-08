// Крок 3 (підтема 3): дрейф vTaskDelay() проти стабільності xTaskDelayUntil().
//
// Дві однакові задачі, обидві хочуть перемикати LED кожні 200 мс, і обидві щоразу
// виконують 7 мс «корисної роботи» (імітація: читання датчика, розрахунок).
//   червоний LED - vTaskDelay(200):          спить 200 мс ПІСЛЯ роботи → період 207 мс;
//   зелений LED  - xTaskDelayUntil(200):     прокидається за сіткою 0, 200, 400... мс.
// На 25-му проході обидві разово «зависають» на 300 мс (перевищення періоду).
// Раз на секунду звіт показує, наскільки кожна задача відстала від ідеальної сітки.
// Це та сама різниця, що в 2.3 між last = now і last += period.

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "practice.h"

#define PERIOD_MS        UINT32_C(200)
#define WORK_US          UINT32_C(7000)    // звичайна робота кожного проходу
#define SPIKE_ITERATION  UINT32_C(25)      // на якому проході разове перевищення
#define SPIKE_US         UINT32_C(300000)  // 300 мс > періоду 200 мс
#define REPORT_PERIOD_MS UINT32_C(1000)
#define US_PER_MS        INT64_C(1000)

// Перевірка під час компіляції: демо має сенс, лише якщо «зависання» довше за період.
static_assert(SPIKE_US > PERIOD_MS * US_PER_MS, "SPIKE_US має перевищувати період");
static_assert(WORK_US < PERIOD_MS * US_PER_MS, "звичайна робота має вкладатися в період");

// У C enum не має власної області видимості (як enum class у C++),
// тому імена констант з префіксом типу: DELAY_RELATIVE, а не просто RELATIVE.
typedef enum {
  DELAY_RELATIVE,  // vTaskDelay: «спати N від зараз»
  DELAY_ABSOLUTE,  // xTaskDelayUntil: «прокинутись у момент T»
} delay_mode_t;

typedef struct {
  const char  *name;
  gpio_num_t   pin;
  delay_mode_t mode;
  BaseType_t   core;  // кожна задача на своєму ядрі: робота однієї не гальмує іншу
  uint8_t      slot;  // індекс у s_stats
} drift_task_config_t;

static const drift_task_config_t TASKS[] = {
    {"vTaskDelay",      LED_RED,   DELAY_RELATIVE, 0, 0},
    {"xTaskDelayUntil", LED_GREEN, DELAY_ABSOLUTE, 1, 1},
};
#define TASK_COUNT (sizeof(TASKS) / sizeof(TASKS[0]))

// Статистика, яку пише задача LED і читає звіт.
// int64_t на 32-бітному ядрі записується за ДВІ інструкції: без захисту звіт може
// прочитати старшу половину старого числа і молодшу нового. Тому копіюємо всю структуру
// під spinlock. На двоядерному S3 саме spinlock (portMUX), бо задачі на різних ядрах.
typedef struct {
  uint32_t iterations;
  int64_t  drift_us;  // наскільки пізніше за ідеальну сітку почався прохід
  uint32_t missed;    // скільки разів xTaskDelayUntil не чекав, бо дедлайн уже минув
} drift_stats_t;

static drift_stats_t s_stats[TASK_COUNT];
static portMUX_TYPE  s_stats_lock = portMUX_INITIALIZER_UNLOCKED;

static void drift_task(void *param) {
  const drift_task_config_t *task = param;
  const int64_t start_us = esp_timer_get_time();
  TickType_t last_wake = xTaskGetTickCount();
  uint32_t iteration = 0;
  uint32_t missed = 0;
  bool on = false;

  for (;;) {
    // Ідеальна сітка: прохід N має починатися в start_us + N * PERIOD.
    const int64_t drift_us =
        esp_timer_get_time() - (start_us + (int64_t)iteration * PERIOD_MS * US_PER_MS);

    // Складений літерал (C99) з designated initializers: вся структура одним присвоєнням.
    taskENTER_CRITICAL(&s_stats_lock);
    s_stats[task->slot] = (drift_stats_t){
        .iterations = iteration, .drift_us = drift_us, .missed = missed};
    taskEXIT_CRITICAL(&s_stats_lock);

    on = !on;
    gpio_set_level(task->pin, on ? 1 : 0);

    // «Корисна робота». Активне очікування навмисно: задача справді зайнята процесором.
    esp_rom_delay_us(iteration == SPIKE_ITERATION ? SPIKE_US : WORK_US);
    ++iteration;

    if (task->mode == DELAY_RELATIVE) {
      // Спати 200 мс ВІД ЗАРАЗ. Час роботи додається до періоду, і так щоразу.
      vTaskDelay(pdMS_TO_TICKS(PERIOD_MS));
    } else {
      // Спати ДО last_wake + 200 мс. last_wake зсувається рівно на період, незалежно від
      // того, скільки тривала робота. Якщо момент уже минув - повертається одразу (pdFALSE).
      if (xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(PERIOD_MS)) == pdFALSE) {
        ++missed;
      }
    }
  }
}

static void report_task(void *arg) {
  (void)arg;
  TickType_t last_wake = xTaskGetTickCount();

  for (;;) {
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(REPORT_PERIOD_MS));

    drift_stats_t copy[TASK_COUNT];
    taskENTER_CRITICAL(&s_stats_lock);
    for (size_t i = 0; i < TASK_COUNT; ++i) {
      copy[i] = s_stats[i];
    }
    taskEXIT_CRITICAL(&s_stats_lock);
    // printf - лише ПІСЛЯ виходу з критичної секції: усередині не можна нічого повільного.

    printf("t=%3" PRIu32 " s", (uint32_t)(pdTICKS_TO_MS(last_wake) / 1000));
    for (size_t i = 0; i < TASK_COUNT; ++i) {
      printf(" | %-15s n=%3" PRIu32 " drift=%+5" PRId64 " ms missed=%" PRIu32,
             TASKS[i].name, copy[i].iterations, copy[i].drift_us / US_PER_MS, copy[i].missed);
    }
    printf("\n");
  }
}

void app_main(void) {
  printf("Practice 6.1 step 3: vTaskDelay vs xTaskDelayUntil, period %" PRIu32
         " ms, work %" PRIu32 " us, spike at #%" PRIu32 "\n",
         PERIOD_MS, WORK_US, SPIKE_ITERATION);

  for (size_t i = 0; i < TASK_COUNT; ++i) {
    led_init(TASKS[i].pin);
    check_task_created(xTaskCreatePinnedToCore(drift_task, TASKS[i].name, LED_STACK_BYTES,
                                               (void *)&TASKS[i], PRIO_LED, NULL,
                                               TASKS[i].core));
  }
  check_task_created(xTaskCreate(report_task, "report", PRINT_STACK_BYTES, NULL,
                                 PRIO_REPORT, NULL));
}
