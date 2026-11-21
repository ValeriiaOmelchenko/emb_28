// Крок 3: Deadlock на двох mutex і два способи його прибрати.
//
// Два спільні ресурси в памʼяті, кожен під своїм mutex:
//   samples - буфер вимірювань;
//   journal - журнал подій.
// Дві задачі на різних ядрах, і кожній потрібні ОБИДВА ресурси одночасно:
//   sampler  (ядро 0): кладе вимірювання в samples і записує подію в journal;
//   archiver (ядро 1): переносить журнал в архів разом зі зрізом samples.
//
// DEADLOCK_MODE:
//   0 - протилежний порядок + portMAX_DELAY. sampler тримає samples і чекає journal,
//       archiver тримає journal і чекає samples. Обидві чекають вічно - DEADLOCK.
//       Лог мовчить, і без монітора (app_main) не видно навіть, що щось не так.
//   1 - протилежний порядок + таймаут і відкат: не вдалось взяти другий mutex за
//       LOCK_TIMEOUT_MS - віддаємо перший і пробуємо пізніше. Система жива, але частина
//       спроб марна. Таймаут не прибирає причину, а лише робить її видимою в лозі.
//   2 - однаковий порядок (завжди samples -> journal) + таймаут. Цикл очікування
//       неможливий, відмов немає. Таймаут лишається як страховка і діагностика.
//
// Чотири умови Коффмана - deadlock можливий, лише коли виконані ВСІ:
//   1. взаємне виключення     - ресурс одночасно в одного власника (так працює mutex);
//   2. утримання й очікування - тримаю один ресурс і чекаю інший;
//   3. без примусового звільнення - забрати mutex у задачі ніхто не може;
//   4. циклічне очікування    - A чекає B, B чекає A.
// Режим 1 ламає умову 2 (відкат - не тримаю перший, поки не дочекалась другого).
// Режим 2 ламає умову 4 (при однаковому порядку цикл не замкнеться).
//
// vTaskDelay всередині критичних секцій тут НАВМИСНО: це «робота з ресурсом», яка розширює
// вікно, щоб deadlock стався за секунду, а не раз на тиждень. У реальному коді критична
// секція коротка - і саме тому такий deadlock проходить усі тести й зʼявляється в полі.

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "practice.h"

#define DEADLOCK_MODE 0 // 0, 1 або 2 - див. опис угорі

#define LOCK_TIMEOUT_MS     UINT32_C(100)
#define MONITOR_PERIOD_MS   UINT32_C(1000)
#define PRIO_WORKER         ((UBaseType_t)5)

// Різні тривалості, щоб задачі не йшли в ногу (інакше в режимі 1 вони можуть роками
// синхронно брати перший mutex і синхронно відступати - це livelock).
#define SAMPLER_FIRST_WORK_MS   UINT32_C(20) // робота з першим ресурсом до взяття другого
#define SAMPLER_BOTH_WORK_MS    UINT32_C(10) // робота з обома ресурсами
#define SAMPLER_PAUSE_MS        UINT32_C(50)
#define ARCHIVER_FIRST_WORK_MS  UINT32_C(30)
#define ARCHIVER_BOTH_WORK_MS   UINT32_C(10)
#define ARCHIVER_PAUSE_MS       UINT32_C(70)

#if DEADLOCK_MODE == 0
#define LOCK_TIMEOUT_TICKS portMAX_DELAY
#else
#define LOCK_TIMEOUT_TICKS pdMS_TO_TICKS(LOCK_TIMEOUT_MS)
#endif

static const char *TAG = "deadlock";

static SemaphoreHandle_t s_samples_mutex;
static SemaphoreHandle_t s_journal_mutex;
static StaticSemaphore_t s_samples_mutex_buf;
static StaticSemaphore_t s_journal_mutex_buf;

// Статистика для монітора. У кожного лічильника один писач (своя задача), а 32-бітне
// вирівняне читання на Xtensa неподільне, тому монітор бачить коректне значення без mutex.
typedef struct {
  volatile uint32_t ok;     // скільки разів взяли обидва ресурси
  volatile uint32_t failed; // скільки разів відступили через таймаут
} worker_stats_t;

static worker_stats_t s_sampler_stats;
static worker_stats_t s_archiver_stats;

// Бере два mutex по черзі. Якщо другий не вдалось взяти - віддає перший (відкат).
// Повертає true, якщо задача тепер власник обох.
static bool take_both(SemaphoreHandle_t first, SemaphoreHandle_t second, uint32_t first_work_ms) {
  if (xSemaphoreTake(first, LOCK_TIMEOUT_TICKS) != pdTRUE) {
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(first_work_ms)); // працюємо з першим ресурсом

  if (xSemaphoreTake(second, LOCK_TIMEOUT_TICKS) != pdTRUE) {
    xSemaphoreGive(first); // не тримаємо перший, поки чекаємо другий
    return false;
  }
  return true;
}

// Віддаємо у зворотному порядку. На коректність не впливає, але це звична конвенція:
// ресурси звільняються як вкладені дужки.
static void give_both(SemaphoreHandle_t first, SemaphoreHandle_t second) {
  xSemaphoreGive(second);
  xSemaphoreGive(first);
}

static void sampler_task(void *arg) {
  (void)arg;
  for (;;) {
    // sampler завжди: samples -> journal
    if (take_both(s_samples_mutex, s_journal_mutex, SAMPLER_FIRST_WORK_MS)) {
      vTaskDelay(pdMS_TO_TICKS(SAMPLER_BOTH_WORK_MS)); // пишемо в обидва буфери
      give_both(s_samples_mutex, s_journal_mutex);
      s_sampler_stats.ok++;
    } else {
      s_sampler_stats.failed++;
      // Лог - ПІСЛЯ відкату, коли жоден mutex не тримаємо: вивід у консоль повільний.
      ESP_LOGW(TAG, "sampler: таймаут, ресурси віддано, спробую пізніше");
    }
    vTaskDelay(pdMS_TO_TICKS(SAMPLER_PAUSE_MS));
  }
}

static void archiver_task(void *arg) {
  (void)arg;
  for (;;) {
#if DEADLOCK_MODE == 2
    // Правило: у ВСІХ задачах однаковий порядок - samples -> journal.
    SemaphoreHandle_t first = s_samples_mutex;
    SemaphoreHandle_t second = s_journal_mutex;
#else
    // Помилка: «мені спершу потрібен журнал» - порядок протилежний до sampler.
    SemaphoreHandle_t first = s_journal_mutex;
    SemaphoreHandle_t second = s_samples_mutex;
#endif
    if (take_both(first, second, ARCHIVER_FIRST_WORK_MS)) {
      vTaskDelay(pdMS_TO_TICKS(ARCHIVER_BOTH_WORK_MS)); // копіюємо журнал і зріз вимірювань
      give_both(first, second);
      s_archiver_stats.ok++;
    } else {
      s_archiver_stats.failed++;
      ESP_LOGW(TAG, "archiver: таймаут, ресурси віддано, спробую пізніше");
    }
    vTaskDelay(pdMS_TO_TICKS(ARCHIVER_PAUSE_MS));
  }
}

// Імʼя задачі-власника mutex або «вільний». Власника памʼятає лише mutex, не семафор.
static const char *holder_name(SemaphoreHandle_t mutex) {
  TaskHandle_t holder = xSemaphoreGetMutexHolder(mutex);
  return (holder != NULL) ? pcTaskGetName(holder) : "вільний";
}

void app_main(void) {
  s_samples_mutex = xSemaphoreCreateMutexStatic(&s_samples_mutex_buf);
  s_journal_mutex = xSemaphoreCreateMutexStatic(&s_journal_mutex_buf);

  ESP_LOGI(TAG, "DEADLOCK_MODE = %d", DEADLOCK_MODE);

  check_task_created(xTaskCreatePinnedToCore(sampler_task, "sampler", TASK_STACK_BYTES, NULL,
                                             PRIO_WORKER, NULL, CORE_0),
                     "sampler");
  check_task_created(xTaskCreatePinnedToCore(archiver_task, "archiver", TASK_STACK_BYTES, NULL,
                                             PRIO_WORKER, NULL, CORE_1),
                     "archiver");

  // Монітор (app_main, пріоритет 1): раз на секунду - прогрес задач і власники mutex.
  // Немає прогресу ні в кого - це і є симптом deadlock: задачі живі, але стоять у Blocked.
  uint32_t prev_total = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(MONITOR_PERIOD_MS));

    const uint32_t total = s_sampler_stats.ok + s_sampler_stats.failed +
                           s_archiver_stats.ok + s_archiver_stats.failed;
    ESP_LOGI(TAG,
             "sampler ok=%" PRIu32 " fail=%" PRIu32 " | archiver ok=%" PRIu32 " fail=%" PRIu32
             " | samples: %s, journal: %s",
             s_sampler_stats.ok, s_sampler_stats.failed, s_archiver_stats.ok,
             s_archiver_stats.failed, holder_name(s_samples_mutex), holder_name(s_journal_mutex));

    if (total == prev_total) {
      ESP_LOGE(TAG, "DEADLOCK: за %" PRIu32 " мс жодного кроку. samples тримає %s, journal тримає %s",
               MONITOR_PERIOD_MS, holder_name(s_samples_mutex), holder_name(s_journal_mutex));
    }
    prev_total = total;
  }
}
