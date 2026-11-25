// Крок 6: автоматичний Light Sleep (Power Management + tickless idle).
// Нічого підключати не треба.
//
// Задача циклічно: 100 мс «працює», потім 2 с чекає. Чекати можна двома способами:
//   BUSY_WAIT 0 - vTaskDelay(): задача в Blocked, на ядрі запускається Idle Task,
//                 і ESP-IDF сам кладе чип у Light Sleep до наступної події;
//   BUSY_WAIT 1 - while (час не вийшов) {}: задача весь час Running,
//                 Idle Task на цьому ядрі не отримує CPU, сну немає.
// Код задачі однаковий, різниця - лише в тому, ЯК вона чекає.
//
// Чип засинає, лише коли ОБИДВА ядра в Idle. Один busy-wait на одному ядрі - і сну немає для всіх.
//
// Що побачимо: мультиметр, а також статистику esp_pm_dump_locks() кожні 5 циклів.
// Рядок SLEEP у «Mode stats» - скільки відсотків часу чип реально спав.
//
// Налаштування (sdkconfig.defaults): CONFIG_PM_ENABLE, CONFIG_FREERTOS_USE_TICKLESS_IDLE,
// CONFIG_PM_PROFILING. Без виклику esp_pm_configure() вони нічого не вмикають.

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "practice.h"

#define BUSY_WAIT 0

#define WORK_MS         UINT32_C(100)
#define WAIT_MS         UINT32_C(2000) // < 5 с, інакше busy-wait ще й розбудить Task Watchdog
#define STATS_EVERY     UINT32_C(5)    // циклів між виводом статистики
#define XTAL_FREQ_MHZ   40             // мінімальна частота для DFS - частота кварцу

static const char *TAG = "pm";

// Тримає CPU зайнятим ms мілісекунд (задача весь час Running).
static void spin_ms(uint32_t ms) {
  const int64_t until_us = esp_timer_get_time() + (int64_t)ms * 1000;
  while (esp_timer_get_time() < until_us) {
  }
}

void app_main(void) {
  const esp_pm_config_t pm_config = {
      .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
      .min_freq_mhz = XTAL_FREQ_MHZ,
      .light_sleep_enable = true, // Idle Task може класти чип у Light Sleep
  };
  ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
  ESP_LOGI(TAG, "PM увімкнено, BUSY_WAIT = %d", BUSY_WAIT);

  for (uint32_t cycle = 1;; cycle++) {
    const int64_t t0_us = esp_timer_get_time();
    spin_ms(WORK_MS); // «корисна робота»: тут CPU справді потрібен

#if BUSY_WAIT
    spin_ms(WAIT_MS); // «чекаю, поки настане час»: CPU на 100%, корисної роботи нуль
#else
    vTaskDelay(pdMS_TO_TICKS(WAIT_MS));
#endif

    ESP_LOGI(TAG, "цикл %" PRIu32 ": %" PRId64 " мс", cycle, (esp_timer_get_time() - t0_us) / 1000);
    if (cycle % STATS_EVERY == 0) {
      esp_pm_dump_locks(stdout);
    }
  }
}
