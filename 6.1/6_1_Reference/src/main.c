// Еталон 6.1 - ДЗ 2.3 (три незалежні LED) на задачах FreeRTOS.
// ESP32-S3-DevKitC-1, ESP-IDF, чистий C17.
//
//   червоний - перемикається кожні 200 мс;
//   зелений  - кожні 500 мс;
//   синій    - кожні 1000 мс.
//
// Кожен LED - окрема задача з однією й тією самою функцією, параметри - запис у static const
// таблиці (pvParameters). Задача перемикає LED і спить до свого наступного моменту
// (xTaskDelayUntil), а не прокидається щотіку перевірити «чи настав час».
// Раз на 10 с задача-звіт друкує, скільки перемикань зробив кожен LED (очікуємо 50 / 20 / 10).

#include <assert.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// --- Залізо (як в еталоні 2.3) ---
// GPIO 4-6: не strapping (0, 3, 45, 46), не USB (19, 20), не UART0-консоль (43, 44),
// не флеш/PSRAM (26-37). Резистори від 3,3 В: червоний 270 Ом, зелений/синій 68 Ом.
#define LED_RED   GPIO_NUM_4
#define LED_GREEN GPIO_NUM_5
#define LED_BLUE  GPIO_NUM_6

// --- Інтервали з 2.3: час між перемиканнями ---
#define RED_TOGGLE_MS    UINT32_C(200)
#define GREEN_TOGGLE_MS  UINT32_C(500)
#define BLUE_TOGGLE_MS   UINT32_C(1000)
#define REPORT_PERIOD_MS UINT32_C(10000)

// Звіт перевіряє «рівно N перемикань за вікно» - це має сенс, лише якщо вікно кратне інтервалам.
static_assert(REPORT_PERIOD_MS % RED_TOGGLE_MS == 0, "вікно звіту має бути кратним 200 мс");
static_assert(REPORT_PERIOD_MS % GREEN_TOGGLE_MS == 0, "вікно звіту має бути кратним 500 мс");
static_assert(REPORT_PERIOD_MS % BLUE_TOGGLE_MS == 0, "вікно звіту має бути кратним 1000 мс");
// З тіком 100 Гц інтервал менше 10 мс перетворився б на 0 тіків.
static_assert(pdMS_TO_TICKS(RED_TOGGLE_MS) > 0, "інтервал коротший за тік FreeRTOS");

// --- Задачі ---
// Усі на ядрі 1: на одному ядрі порядок виконання задач, що прокинулись на тому самому тіку,
// визначає пріоритет. Тоді звіт завжди бачить перемикання цього тіку, і лічильники
// стабільно 50 / 20 / 10, а не 49 / 51 залежно від того, яке ядро встигло першим.
#define APP_CORE ((BaseType_t)1)

// В ESP-IDF стек задачі - у БАЙТАХ (у «ванільному» FreeRTOS на STM32 - у словах по 4 байти).
#define LED_STACK_BYTES    UINT32_C(2048)  // лише GPIO і xTaskDelayUntil, без printf
#define REPORT_STACK_BYTES UINT32_C(3072)  // printf сам зʼїдає 1,5-2 КБ стеку

// Більше число = важливіше. Idle = 0. Блимання важливіше за звіт: звіт може почекати.
#define PRIO_REPORT ((UBaseType_t)(tskIDLE_PRIORITY + 1))
#define PRIO_LED    ((UBaseType_t)(tskIDLE_PRIORITY + 2))
static_assert(PRIO_REPORT < PRIO_LED, "звіт має бути менш важливим за LED");

// --- Таблиця процесів ---
// static const -> лежить у флеші, задачі її лише читають. Четвертий LED = один рядок.
typedef struct {
  const char *name;  // і імʼя задачі: не довше 15 символів (configMAX_TASK_NAME_LEN - 1)
  gpio_num_t  pin;
  uint32_t    toggle_ms;
} blinker_t;

static const blinker_t BLINKERS[] = {
    {"red",   LED_RED,   RED_TOGGLE_MS},
    {"green", LED_GREEN, GREEN_TOGGLE_MS},
    {"blue",  LED_BLUE,  BLUE_TOGGLE_MS},
};
// sizeof рахується під час компіляції, тож LED_COUNT годиться для розміру масивів.
#define LED_COUNT (sizeof(BLINKERS) / sizeof(BLINKERS[0]))

// Лічильники перемикань. У кожного один писач (своя задача LED), читає лише звіт,
// і звіт їх не скидає, а рахує різницю сам. _Atomic - щоб звіт ніколи не прочитав
// «пів числа» і щоб компілятор не тримав значення в регістрі.
static _Atomic uint32_t s_toggles[LED_COUNT];

// Спільна точка відліку для всіх задач: сітки 200 / 500 / 1000 мс збігаються, раз на
// секунду всі три LED перемикаються на одному тіку (як «один startMs» у 2.3).
// app_main пише її ДО створення задач - створення задачі є точкою синхронізації,
// тож задачі бачать уже заповнене значення.
static TickType_t s_start_tick;

static uint32_t ticks_to_ms(TickType_t ticks) {
  return (uint32_t)pdTICKS_TO_MS(ticks);
}

// ---------------------------------------------------------------------------
// Задача LED: одна функція, три екземпляри
// ---------------------------------------------------------------------------
static void blink_task(void *arg) {
  // У C void * приводиться до вказівника неявно. const не губимо: запис у static const
  // таблицю на ESP32 - це запис у флеш і аварія StoreProhibited.
  const blinker_t *led = arg;
  const size_t idx = (size_t)(led - BLINKERS);  // номер запису в таблиці = індекс лічильника
  const TickType_t period = pdMS_TO_TICKS(led->toggle_ms);

  TickType_t last_wake = s_start_tick;
  bool on = false;

  for (;;) {
    // Спимо (Blocked) до моменту last_wake + period. xTaskDelayUntil рахує від
    // ЗАПЛАНОВАНОГО моменту, тому час роботи тіла циклу не додається до періоду:
    // це «last += period» з 2.3. vTaskDelay(period) = «last = now» - ритм дрейфує.
    xTaskDelayUntil(&last_wake, period);

    on = !on;
    gpio_set_level(led->pin, on ? 1U : 0U);
    atomic_fetch_add_explicit(&s_toggles[idx], 1, memory_order_relaxed);
  }
}

// ---------------------------------------------------------------------------
// Звіт: доказ, що таймінг точний, - числом, а не «на око»
// ---------------------------------------------------------------------------
static void report_task(void *arg) {
  (void)arg;
  uint32_t previous[LED_COUNT] = {0};
  TickType_t last_wake = s_start_tick;

  for (;;) {
    // Та сама сітка, що й у LED. На тику 10000 мс прокидаються і LED, і звіт, але LED
    // важливіші й на тому самому ядрі - вони перемкнуться першими, і звіт їх порахує.
    xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(REPORT_PERIOD_MS));

    bool all_ok = true;
    printf("[%7" PRIu32 " ms] toggles:", ticks_to_ms(last_wake - s_start_tick));
    for (size_t i = 0; i < LED_COUNT; ++i) {
      const uint32_t now = atomic_load_explicit(&s_toggles[i], memory_order_relaxed);
      const uint32_t delta = now - previous[i];  // беззнакова різниця: ок і при переповненні
      previous[i] = now;
      printf(" %s=%" PRIu32, BLINKERS[i].name, delta);
      if (delta != REPORT_PERIOD_MS / BLINKERS[i].toggle_ms) {
        all_ok = false;
      }
    }
    printf("  %s\n", all_ok ? "OK" : "MISMATCH");
  }
}

// ---------------------------------------------------------------------------
// Старт
// ---------------------------------------------------------------------------

// Три виходи одним gpio_config: явний режим, без підтяжок, без переривань, стартовий рівень 0.
static void leds_init(void) {
  uint64_t mask = 0;
  for (size_t i = 0; i < LED_COUNT; ++i) {
    mask |= 1ULL << BLINKERS[i].pin;
  }
  const gpio_config_t conf = {
      .pin_bit_mask = mask,
      .mode         = GPIO_MODE_OUTPUT,
      .pull_up_en   = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type    = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&conf));
  for (size_t i = 0; i < LED_COUNT; ++i) {
    ESP_ERROR_CHECK(gpio_set_level(BLINKERS[i].pin, 0));
  }
}

// xTaskCreate мовчки повертає помилку, якщо не вистачило heap під стек, - і «один LED
// чомусь не блимає». Перевіряємо кожну задачу через != pdPASS і зупиняємось з назвою:
// напівживий пристрій гірший за той, що чесно впав з повідомленням.
static void create_task_or_abort(TaskFunction_t fn, const char *name, uint32_t stack_bytes,
                                 void *arg, UBaseType_t prio) {
  if (xTaskCreatePinnedToCore(fn, name, stack_bytes, arg, prio, NULL, APP_CORE) != pdPASS) {
    printf("ERROR: task %s not created\n", name);
    abort();
  }
}

void app_main(void) {
  printf("=== 6.1: red %" PRIu32 " ms, green %" PRIu32 " ms, blue %" PRIu32 " ms ===\n",
         RED_TOGGLE_MS, GREEN_TOGGLE_MS, BLUE_TOGGLE_MS);

  leds_init();
  s_start_tick = xTaskGetTickCount();

  for (size_t i = 0; i < LED_COUNT; ++i) {
    // pvParameters - це void *, тому const знімаємо явно лише тут; задача поверне його
    // назад (const blinker_t *) і дані лише читатиме.
    create_task_or_abort(blink_task, BLINKERS[i].name, LED_STACK_BYTES, (void *)&BLINKERS[i],
                         PRIO_LED);
  }
  create_task_or_abort(report_task, "report", REPORT_STACK_BYTES, NULL, PRIO_REPORT);

  // app_main може повернутись: задачі живуть далі, задачу main ESP-IDF видалить сам
  // і звільнить її стек. (В Arduino аналог - vTaskDelete(NULL) у кінці setup().)
}
