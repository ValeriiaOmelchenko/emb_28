// Еталон 6.2 - FreeRTOS: задачі та планувальник.
// ESP32-S3-DevKitC-1, ESP-IDF, чистий C17.
//
// Чотири задачі:
//   Sensor Reader (Normal)   - строго кожні 500 мс через xTaskDelayUntil
//   Event Trigger (Low)      - vTaskDelay 3 с, потім створює Worker
//   Worker        (High)     - друкує і видаляє себе через vTaskDelete(NULL)
//   Maintenance   (Realtime) - раз на 10 с (теж xTaskDelayUntil) призупиняє датчик на 2 с
//
// Усі задачі - на ядрі 1. На одному ядрі видно витіснення за пріоритетом (на двох ядрах S3
// Realtime і Normal просто працювали б паралельно), і Maintenance не може призупинити
// датчик у ту мить, коли той на іншому ядрі посеред printf.

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_system.h"
#include "esp_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// --- Ядро і стек ---
#define APP_CORE         1
// В ESP-IDF стек задачі - у БАЙТАХ. printf зʼїдає 1,5-2 КБ, тому 2048 - впритул.
// Реальний запас друкує Maintenance (uxTaskGetStackHighWaterMark, теж у байтах).
#define TASK_STACK_BYTES UINT32_C(3072)

// --- Пріоритети (більше число = важливіше). Idle = 0. ---
#define PRIO_LOW      ((UBaseType_t)(tskIDLE_PRIORITY + 1))
#define PRIO_NORMAL   ((UBaseType_t)(tskIDLE_PRIORITY + 2))
#define PRIO_HIGH     ((UBaseType_t)(tskIDLE_PRIORITY + 3))
#define PRIO_REALTIME ((UBaseType_t)(tskIDLE_PRIORITY + 4))
static_assert(PRIO_LOW < PRIO_NORMAL && PRIO_NORMAL < PRIO_HIGH && PRIO_HIGH < PRIO_REALTIME,
              "порядок пріоритетів з умови: Low < Normal < High < Realtime");
// Системні задачі ESP-IDF (esp_timer, ipc) мають бути важливішими за наші.
static_assert(PRIO_REALTIME < ESP_TASK_TIMER_PRIO, "Realtime не має перебивати esp_timer");

// --- Час ---
#define SENSOR_PERIOD   pdMS_TO_TICKS(500)
#define EVENT_SLEEP     pdMS_TO_TICKS(3000)
#define MAINT_PERIOD    pdMS_TO_TICKS(10000)
#define MAINT_PAUSE     pdMS_TO_TICKS(2000)
// Скільки Maintenance чекає, поки датчик дійде до безпечної точки (див. wait_sensor_idle).
#define SAFE_POINT_WAIT pdMS_TO_TICKS(100)
static_assert(MAINT_PAUSE < MAINT_PERIOD, "пауза має бути коротшою за період обслуговування");
static_assert(SENSOR_PERIOD > 0, "з тіком 100 Гц ще працює, але менше 10 мс - вже 0 тіків");

// Хендли пише app_main ДО створення Maintenance - створення задачі є точкою синхронізації,
// тож Maintenance бачить уже заповнені значення.
static TaskHandle_t s_sensor;
static TaskHandle_t s_event;

// Час з моменту старту планувальника, мс. Лише для міток у лозі: по ним видно періоди.
static uint32_t now_ms(void) {
  return (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
}

// ---------------------------------------------------------------------------
// 1. Sensor Reader
// ---------------------------------------------------------------------------
static void sensor_task(void *arg) {
  (void)arg;
  TickType_t last_wake = xTaskGetTickCount();

  for (;;) {
    // xTaskDelayUntil рахує від ЗАПЛАНОВАНОГО моменту, а не від «зараз»: час роботи
    // і витіснення не накопичуються, період рівно 500 мс.
    xTaskDelayUntil(&last_wake, SENSOR_PERIOD);

    // last_wake тепер = запланований момент цього слоту. Якщо прокинулись на цілий період
    // пізніше - нас ставили на паузу (vTaskSuspend). Без цієї перевірки xTaskDelayUntil
    // з ціллю в минулому повертався б одразу, і після resume вилетіла б пачка з 4-5 рядків
    // за одну мілісекунду. Різниця беззнакова - коректна і при переповненні лічильника тіків.
    const TickType_t now = xTaskGetTickCount();
    const TickType_t late = now - last_wake;
    if (late >= SENSOR_PERIOD) {
      // Пропущені слоти не доганяємо, а стаємо на найближчу точку старої сітки 500 мс.
      // continue: і не читаємо датчик поза розкладом.
      last_wake = now - (late % SENSOR_PERIOD);
      continue;
    }

    printf("%6" PRIu32 " ms  [Sensor] Data read...\n", now_ms());
  }
}

// ---------------------------------------------------------------------------
// 3. Worker - живе рівно одну подію
// ---------------------------------------------------------------------------
static void worker_task(void *arg) {
  (void)arg;
  printf("%6" PRIu32 " ms  [Worker] Processing event...\n", now_ms());
  // Вийти через return або } з функції задачі не можна: у FreeRTOS це assert/аварія.
  // vTaskDelete(NULL) видаляє саму себе; стек і TCB звільнить Idle-задача ядра 1.
  vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// 2. Event Trigger
// ---------------------------------------------------------------------------
static void event_trigger_task(void *arg) {
  (void)arg;
  for (;;) {
    // Тут за умовою саме vTaskDelay: подія «непередбачувана», строга сітка їй не потрібна.
    vTaskDelay(EVENT_SLEEP);

    printf("%6" PRIu32 " ms  [Event] Event occurred, creating Worker\n", now_ms());
    // Worker важливіший (High > Low) і на тому самому ядрі: він витіснить нас одразу
    // всередині xTaskCreatePinnedToCore і надрукує свій рядок раніше, ніж ми повернемось.
    if (xTaskCreatePinnedToCore(worker_task, "worker", TASK_STACK_BYTES, NULL, PRIO_HIGH, NULL,
                                APP_CORE) != pdPASS) {
      // Не вистачило heap. Систему не зупиняємо, але подія не губиться мовчки.
      printf("%6" PRIu32 " ms  [Event] ERROR: Worker not created (heap free %" PRIu32 " B)\n",
             now_ms(), esp_get_free_heap_size());
    }
  }
}

// ---------------------------------------------------------------------------
// 4. Maintenance
// ---------------------------------------------------------------------------

// vTaskSuspend зупиняє задачу будь-де. Якщо датчик саме в printf, він тримає lock stdout,
// і наступний printf у самій Maintenance чекав би на нього вічно, а vTaskResume стоїть
// уже після нього - обидві задачі зависли б. Тому призупиняємо датчик лише в безпечній
// точці: коли він заблокований (спить у xTaskDelayUntil). На одному ядрі Maintenance -
// найважливіша, тож між перевіркою і vTaskSuspend датчик виконатись не встигне.
// Повне рішення для спільного ресурсу - мʼютекс (наступні уроки).
static bool wait_sensor_idle(void) {
  const TickType_t start = xTaskGetTickCount();
  while (eTaskGetState(s_sensor) != eBlocked) {
    if ((TickType_t)(xTaskGetTickCount() - start) >= SAFE_POINT_WAIT) {
      return false;
    }
    vTaskDelay(1);  // віддаємо ядро: датчик дописує свій рядок і засинає
  }
  return true;
}

static void report_health(void) {
  // Запас стеку - мінімум вільного місця за весь час роботи. Так обґрунтовується 3072,
  // а не «на око». Вільний heap не має зменшуватись: Worker-и не течуть.
  printf("%6" PRIu32 " ms  [Maintenance] stack free: sensor %u, event %u, maintenance %u B;"
         " heap free %" PRIu32 " B\n",
         now_ms(), (unsigned)uxTaskGetStackHighWaterMark(s_sensor),
         (unsigned)uxTaskGetStackHighWaterMark(s_event),
         (unsigned)uxTaskGetStackHighWaterMark(NULL), esp_get_free_heap_size());
}

static void maintenance_task(void *arg) {
  (void)arg;
  TickType_t last_wake = xTaskGetTickCount();

  for (;;) {
    // «Раз на 10 с» - теж строга періодичність. З vTaskDelay(10000) + vTaskDelay(2000)
    // період був би 12 с; з xTaskDelayUntil пауза всередині тіла сітку не зсуває.
    xTaskDelayUntil(&last_wake, MAINT_PERIOD);

    if (!wait_sensor_idle()) {
      printf("%6" PRIu32 " ms  [Maintenance] Sensor busy, maintenance skipped\n", now_ms());
      continue;
    }

    vTaskSuspend(s_sensor);
    printf("%6" PRIu32 " ms  [Maintenance] System paused for 2 seconds.\n", now_ms());
    report_health();
    vTaskDelay(MAINT_PAUSE);  // тут саме vTaskDelay: 2 с від цього моменту
    vTaskResume(s_sensor);
  }
}

// ---------------------------------------------------------------------------
// Старт
// ---------------------------------------------------------------------------

// Перевіряємо кожну задачу окремо через != pdPASS.
// Пастка: ok &= xTaskCreate(...) не ловить помилку, бо pdPASS = 1, а помилка = -1
// (усі біти одиниці), і 1 & -1 == 1.
static void create_task(TaskFunction_t fn, const char *name, UBaseType_t prio,
                        TaskHandle_t *handle) {
  if (xTaskCreatePinnedToCore(fn, name, TASK_STACK_BYTES, NULL, prio, handle, APP_CORE) !=
      pdPASS) {
    printf("ERROR: task %s not created\n", name);
    abort();
  }
}

void app_main(void) {
  printf("=== 6.2: Sensor / Event / Worker / Maintenance, ядро %d ===\n", APP_CORE);

  // Датчик - першим: його хендл потрібен Maintenance. Якби він не створився і лишився NULL,
  // vTaskSuspend(NULL) призупинив би САМУ Maintenance, і система мовчки завмерла б.
  create_task(sensor_task, "sensor", PRIO_NORMAL, &s_sensor);
  create_task(event_trigger_task, "event", PRIO_LOW, &s_event);
  create_task(maintenance_task, "maintenance", PRIO_REALTIME, NULL);
  // app_main може повернутись: задачі живуть далі, а задачу main ESP-IDF видалить сам.
}
