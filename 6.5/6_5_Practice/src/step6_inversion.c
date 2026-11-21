// Крок 6: Priority Inversion і Priority Inheritance.
//
// Три задачі на ОДНОМУ ядрі (ядро 1, як на одноядерному МК), один спільний ресурс:
//   L (low,  пріоритет 2) - бере ресурс і працює з ним LOW_WORK_MS;
//   H (high, пріоритет 4) - критична задача, їй теж потрібен ресурс;
//   M (mid,  пріоритет 3) - ресурс їй не потрібен, вона просто довго рахує (MID_WORK_MS).
//
// Сценарій раунду (керує controller, пріоритет 5):
//   1. L бере ресурс і починає працювати.
//   2. Прокидається H, хоче ресурс - чекає L. Поки що нормально: чекати L_WORK_MS - очікувано.
//   3. Прокидається M. Вона важливіша за L, тому витісняє її. L стоїть, ресурс не віддає,
//      і H (найважливіша!) чекає, поки досчитає M, якій ресурс навіть не потрібен.
//      Це і є інверсія: фактично H виконується з пріоритетом нижчим за M.
//
// Раунди чергують «замок» на ресурсі:
//   бінарний семафор - власника немає, FreeRTOS не знає, кого «підганяти»:
//                      H чекає ≈ LOW_WORK_MS + MID_WORK_MS;
//   mutex            - власник відомий (pxMutexHolder). Коли H чекає mutex, ядро тимчасово
//                      піднімає пріоритет L до 4 (Priority Inheritance). M уже не може
//                      витіснити L, L швидко дорахує і віддасть mutex: H чекає ≈ LOW_WORK_MS.
//
// Mars Pathfinder, 1997: L - збір метеоданих, M - звʼязок, H - керування шиною. Watchdog бачив,
// що H не встигає, і перезавантажував марсохід. Виправили з Землі, увімкнувши успадкування
// пріоритету в mutex.
//
// В ESP-IDF успадкування в mutex увімкнене завжди. В CMSIS-OS v2 (STM32CubeMX) - лише з
// атрибутом osMutexPrioInherit, і CubeMX сам його не ставить.

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "practice.h"

#define LOW_WORK_MS        UINT32_C(50)
#define MID_WORK_MS        UINT32_C(300)
#define L_HEADSTART_MS     UINT32_C(20)   // L встигає взяти ресурс до появи H і M
#define H_LOCK_TIMEOUT_MS  UINT32_C(1000) // дедлайн H: не дочекалась - аварія, а не вічне очікування
#define ROUND_TIMEOUT_MS   UINT32_C(2000)
#define ROUND_PAUSE_MS     UINT32_C(2000)

#define PRIO_LOW        ((UBaseType_t)2)
#define PRIO_MID        ((UBaseType_t)3)
#define PRIO_HIGH       ((UBaseType_t)4)
#define PRIO_CONTROLLER ((UBaseType_t)5)

static const char *TAG = "inversion";

// Два «замки» на той самий ресурс. У раунді використовується один із них.
static SemaphoreHandle_t s_lock_binary;
static SemaphoreHandle_t s_lock_mutex;
static StaticSemaphore_t s_lock_binary_buf;
static StaticSemaphore_t s_lock_mutex_buf;

// Замок поточного раунду. Пише лише controller між раундами, коли L і H сплять.
static SemaphoreHandle_t s_lock;

// Команди старту кожній задачі і «раунд завершено» від H.
static SemaphoreHandle_t s_go_low, s_go_mid, s_go_high, s_high_done;
static StaticSemaphore_t s_go_low_buf, s_go_mid_buf, s_go_high_buf, s_high_done_buf;

// Результати раунду. Кожне поле пише одна задача, controller читає після s_high_done.
static int64_t s_high_wait_ms;
static UBaseType_t s_low_prio_seen; // пріоритет L наприкінці роботи з ресурсом
static bool s_high_got_lock;

static void low_task(void *arg) {
  (void)arg;
  for (;;) {
    xSemaphoreTake(s_go_low, portMAX_DELAY); // чекаємо команду - див. крок 1
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(H_LOCK_TIMEOUT_MS)) != pdTRUE) {
      ESP_LOGE(TAG, "L: не взяла ресурс");
      continue;
    }
    busy_work_ms(LOW_WORK_MS);                 // робота з ресурсом, займає CPU
    s_low_prio_seen = uxTaskPriorityGet(NULL); // з mutex тут буде 4, а не 2
    xSemaphoreGive(s_lock);
  }
}

static void mid_task(void *arg) {
  (void)arg;
  for (;;) {
    xSemaphoreTake(s_go_mid, portMAX_DELAY);
    busy_work_ms(MID_WORK_MS); // довга робота, ресурс не потрібен
  }
}

static void high_task(void *arg) {
  (void)arg;
  for (;;) {
    xSemaphoreTake(s_go_high, portMAX_DELAY);
    const int64_t t0_us = esp_timer_get_time();
    s_high_got_lock = (xSemaphoreTake(s_lock, pdMS_TO_TICKS(H_LOCK_TIMEOUT_MS)) == pdTRUE);
    s_high_wait_ms = (esp_timer_get_time() - t0_us) / 1000;
    if (s_high_got_lock) {
      xSemaphoreGive(s_lock); // сама робота H коротка, тут її не імітуємо
    }
    xSemaphoreGive(s_high_done);
  }
}

static void controller_task(void *arg) {
  (void)arg;
  for (uint32_t round = 0;; round++) {
    const bool use_mutex = (round % 2U) == 1U;
    s_lock = use_mutex ? s_lock_mutex : s_lock_binary;

    xSemaphoreGive(s_go_low);
    vTaskDelay(pdMS_TO_TICKS(L_HEADSTART_MS)); // L бере ресурс і працює
    xSemaphoreGive(s_go_high);                 // H одразу блокується на ресурсі
    xSemaphoreGive(s_go_mid);                  // M витісняє L (або ні, якщо L успадкувала 4)

    if (xSemaphoreTake(s_high_done, pdMS_TO_TICKS(ROUND_TIMEOUT_MS)) != pdTRUE) {
      ESP_LOGE(TAG, "H не завершила раунд за %" PRIu32 " мс", ROUND_TIMEOUT_MS);
    } else {
      // Дочекаємось, поки M закінчить, щоб наступний раунд почався з тихого ядра.
      vTaskDelay(pdMS_TO_TICKS(MID_WORK_MS));
      const char *lock_name = use_mutex ? "mutex           " : "бінарний семафор";
      if (!s_high_got_lock) {
        ESP_LOGE(TAG, "%s: H пропустила дедлайн %" PRIu32 " мс", lock_name, H_LOCK_TIMEOUT_MS);
      } else if (use_mutex) {
        ESP_LOGI(TAG, "%s: H чекала %3" PRId64 " мс, пріоритет L під час роботи: %u",
                 lock_name, s_high_wait_ms, (unsigned)s_low_prio_seen);
      } else {
        ESP_LOGW(TAG, "%s: H чекала %3" PRId64 " мс, пріоритет L під час роботи: %u",
                 lock_name, s_high_wait_ms, (unsigned)s_low_prio_seen);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(ROUND_PAUSE_MS));
  }
}

void app_main(void) {
  // Бінарний семафор створюється ПОРОЖНІМ (0). Щоб він працював як замок, його треба
  // один раз «відкрити» - Give. Mutex створюється вже вільним.
  s_lock_binary = xSemaphoreCreateBinaryStatic(&s_lock_binary_buf);
  xSemaphoreGive(s_lock_binary);
  s_lock_mutex = xSemaphoreCreateMutexStatic(&s_lock_mutex_buf);

  s_go_low = xSemaphoreCreateBinaryStatic(&s_go_low_buf);
  s_go_mid = xSemaphoreCreateBinaryStatic(&s_go_mid_buf);
  s_go_high = xSemaphoreCreateBinaryStatic(&s_go_high_buf);
  s_high_done = xSemaphoreCreateBinaryStatic(&s_high_done_buf);

  // Усі на ядрі 1. Ядро 0 зайняте системними задачами ESP-IDF, а app_main теж на ньому.
  check_task_created(xTaskCreatePinnedToCore(low_task, "L", TASK_STACK_BYTES, NULL, PRIO_LOW, NULL, CORE_1), "L");
  check_task_created(xTaskCreatePinnedToCore(mid_task, "M", TASK_STACK_BYTES, NULL, PRIO_MID, NULL, CORE_1), "M");
  check_task_created(xTaskCreatePinnedToCore(high_task, "H", TASK_STACK_BYTES, NULL, PRIO_HIGH, NULL, CORE_1), "H");
  check_task_created(xTaskCreatePinnedToCore(controller_task, "controller", TASK_STACK_BYTES, NULL,
                                             PRIO_CONTROLLER, NULL, CORE_1),
                     "controller");

  ESP_LOGI(TAG, "L: робота %" PRIu32 " мс, M: %" PRIu32 " мс. Без успадкування H чекає ~%" PRIu32
                " мс, з успадкуванням ~%" PRIu32 " мс",
           LOW_WORK_MS, MID_WORK_MS, LOW_WORK_MS - L_HEADSTART_MS + MID_WORK_MS,
           LOW_WORK_MS - L_HEADSTART_MS);
}
