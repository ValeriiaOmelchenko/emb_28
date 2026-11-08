/*
 * 6.2 - FreeRTOS: задачі та планувальник під SEGGER SystemView.
 *
 * Сценарій практики 6.2, який зручно розглядати на таймлайні:
 *   Heartbeat (пріоритет 1, ядро 0) - коротка робота раз на 500 мс;
 *   HeavyMath (пріоритет 2, ядро 1) - "важкі обчислення" порціями по 20 мс;
 *   Monitor   (пріоритет 3, ядро 1) - строго періодична задача, 100 мс.
 *
 * HEAVY_YIELDS = 0: HeavyMath ніколи не блокується. На ядрі 1 видно,
 *   що IDLE1 не отримує часу взагалі (голодування), а через ~5 с task
 *   watchdog пише в лог "IDLE1 did not reset the watchdog".
 *   Monitor усе одно виконується вчасно, бо має вищий пріоритет (preemption).
 * HEAVY_YIELDS = 1: після кожної порції HeavyMath віддає процесор на 1 тік,
 *   і IDLE1 з'являється на таймлайні.
 *
 * Ділянки "корисної роботи" позначено маркерами SystemView (MarkStart/MarkStop),
 * на таймлайні це видно як інтервали з номером маркера.
 */
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#if CONFIG_APPTRACE_SV_ENABLE
#include "SEGGER_SYSVIEW.h"
#define SV_MARK_START(id) SEGGER_SYSVIEW_MarkStart(id)
#define SV_MARK_STOP(id)  SEGGER_SYSVIEW_MarkStop(id)
#else
/* Без SystemView маркери просто зникають, код збирається і так. */
#define SV_MARK_START(id) ((void)(id))
#define SV_MARK_STOP(id)  ((void)(id))
#endif

#define HEAVY_YIELDS 1

/* Номери маркерів на таймлайні SystemView. */
enum {
    MARK_HEARTBEAT = 1,
    MARK_HEAVY     = 2,
    MARK_MONITOR   = 3,
};

#define HEARTBEAT_PERIOD_MS UINT32_C(500)
#define HEARTBEAT_WORK_US   UINT32_C(2000)
#define HEAVY_CHUNK_US      UINT32_C(20000)
#define MONITOR_PERIOD_MS   UINT32_C(100)
#define MONITOR_WORK_US     UINT32_C(3000)
#define MONITOR_LOG_EVERY   UINT32_C(10)

#define PRIO_HEARTBEAT ((UBaseType_t)1)
#define PRIO_HEAVY     ((UBaseType_t)3)
#define PRIO_MONITOR   ((UBaseType_t)3)

#define STACK_SIZE UINT32_C(3072)

static_assert(MONITOR_WORK_US < MONITOR_PERIOD_MS * 1000U,
              "Робота Monitor має вкладатися в його період");

static const char *TAG = "SV_6_2";

/*
 * Імітація роботи: тримає CPU зайнятим заданий час (busy-wait).
 * Навмисно не vTaskDelay - нам потрібне саме навантаження на ядро.
 */
static void busy_work_us(uint32_t us)
{
    esp_rom_delay_us(us);
}

static void heartbeat_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        SV_MARK_START(MARK_HEARTBEAT);
        busy_work_us(HEARTBEAT_WORK_US);
        SV_MARK_STOP(MARK_HEARTBEAT);

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS));
    }
}

static void heavy_math_task(void *arg)
{
    (void)arg;

    for (;;) {
        SV_MARK_START(MARK_HEAVY);
        busy_work_us(HEAVY_CHUNK_US);
        SV_MARK_STOP(MARK_HEAVY);

#if HEAVY_YIELDS
        vTaskDelay(1);
#endif
    }
}

static void monitor_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t cycles = 0;

    for (;;) {
        SV_MARK_START(MARK_MONITOR);
        busy_work_us(MONITOR_WORK_US);
        SV_MARK_STOP(MARK_MONITOR);

        cycles++;
        if (cycles % MONITOR_LOG_EVERY == 0U) {
            ESP_LOGI(TAG, "Monitor OK, core %d, cycles %lu",
                     xPortGetCoreID(), (unsigned long)cycles);
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(MONITOR_PERIOD_MS));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Start. HEAVY_YIELDS=%d", HEAVY_YIELDS);

    BaseType_t ok = pdPASS;
    ok &= xTaskCreatePinnedToCore(heartbeat_task, "Heartbeat", STACK_SIZE, NULL,
                                  PRIO_HEARTBEAT, NULL, 0);
    ok &= xTaskCreatePinnedToCore(heavy_math_task, "HeavyMath", STACK_SIZE, NULL,
                                  PRIO_HEAVY, NULL, 1);
    ok &= xTaskCreatePinnedToCore(monitor_task, "Monitor", STACK_SIZE, NULL,
                                  PRIO_MONITOR, NULL, 1);

    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Task creation failed (out of memory?)");
    }
}
