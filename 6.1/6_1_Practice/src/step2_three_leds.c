// Крок 2 (підтема 3): задача 2.3 на FreeRTOS.
//
// У 2.3 три LED жили в одному суперлупі, і кожен прохід питав «чи настав час?».
// Тут кожен LED - окрема задача: перемкнула LED і заснула на свій інтервал.
// Одна функція задачі, три екземпляри з різними параметрами (pvParameters).
// Раз на 10 с задача-звіт друкує, скільки разів перемкнувся кожен LED
// (очікуємо ≈ 50 / 20 / 10, як у 2.3) і скільки стеку лишилось у кожної задачі.

#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "practice.h"

#define REPORT_PERIOD_MS UINT32_C(10000)

// Таблиця процесів з 2.3. static const → лежить у флеші, задачі її лише читають.
typedef struct {
  const char *name;
  gpio_num_t  pin;
  uint32_t    toggle_ms;
} blinker_t;

static const blinker_t BLINKERS[] = {
    {"red",   LED_RED,   RED_TOGGLE_MS},
    {"green", LED_GREEN, GREEN_TOGGLE_MS},
    {"blue",  LED_BLUE,  BLUE_TOGGLE_MS},
};
// sizeof рахується під час компіляції, тож LED_COUNT годиться і для розміру масивів.
#define LED_COUNT (sizeof(BLINKERS) / sizeof(BLINKERS[0]))

// Лічильники перемикань: пише лише «своя» задача LED, читає задача-звіт.
// _Atomic (C11, <stdatomic.h>) гарантує, що звіт ніколи не прочитає «пів числа».
// Звіт лічильники не скидає (рахує різницю сам): у кожної змінної один писач, без гонок.
// static-масиви в C обнуляються самі, явний ініціалізатор не потрібен.
static _Atomic uint32_t s_toggles[LED_COUNT];
static TaskHandle_t     s_handles[LED_COUNT];

static void blink_task(void *param) {
  const blinker_t *led = param;  // у C void* приводиться до будь-якого вказівника неявно
  const size_t idx = (size_t)(led - BLINKERS);  // номер запису в таблиці
  bool on = false;

  for (;;) {
    on = !on;
    gpio_set_level(led->pin, on ? 1 : 0);
    atomic_fetch_add_explicit(&s_toggles[idx], 1, memory_order_relaxed);
    // Blocked на toggle_ms: процесор вільний для інших задач, нічого не «крутиться».
    vTaskDelay(pdMS_TO_TICKS(led->toggle_ms));
  }
}

static void report_task(void *arg) {
  (void)arg;
  uint32_t previous[LED_COUNT] = {0};
  TickType_t last_wake = xTaskGetTickCount();

  for (;;) {
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(REPORT_PERIOD_MS));

    printf("[%7" PRIu32 " ms] toggles:", (uint32_t)pdTICKS_TO_MS(last_wake));
    for (size_t i = 0; i < LED_COUNT; ++i) {
      const uint32_t now = atomic_load_explicit(&s_toggles[i], memory_order_relaxed);
      printf(" %s=%" PRIu32, BLINKERS[i].name, now - previous[i]);
      previous[i] = now;
    }

    // Скільки байтів стеку задача НЕ використала за весь час роботи (в ESP-IDF - байти).
    // Якщо тут одиниці-десятки байтів - стек от-от переповниться.
    printf(" | free stack, B:");
    for (size_t i = 0; i < LED_COUNT; ++i) {
      printf(" %s=%u", BLINKERS[i].name, (unsigned)uxTaskGetStackHighWaterMark(s_handles[i]));
    }
    printf(" report=%u\n", (unsigned)uxTaskGetStackHighWaterMark(NULL));
  }
}

void app_main(void) {
  printf("Practice 6.1 step 2: 2.3 on FreeRTOS tasks\n");

  for (size_t i = 0; i < LED_COUNT; ++i) {
    led_init(BLINKERS[i].pin);
    // pvParameters - це void*, тому знімаємо const явним приведенням. Задача дані лише читає.
    check_task_created(xTaskCreate(blink_task, BLINKERS[i].name, LED_STACK_BYTES,
                                   (void *)&BLINKERS[i], PRIO_LED, &s_handles[i]));
  }
  check_task_created(xTaskCreate(report_task, "report", PRINT_STACK_BYTES, NULL,
                                 PRIO_REPORT, NULL));
}
