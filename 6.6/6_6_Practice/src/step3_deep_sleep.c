// Крок 3: Deep Sleep. Прокидаємось таймером кожні 5 с.
// Нічого підключати не треба.
//
// Що побачимо:
//   - кожне пробудження - це перезапуск: знову лог завантажувача і app_main() з першого рядка;
//   - s_boot_count (RTC_DATA_ATTR, лежить у RTC SRAM) росте: 1, 2, 3...
//   - s_ram_count (звичайна RAM) щоразу 1: digital-домен був знеструмлений, RAM обнулилась;
//   - мультиметр: між пробудженнями струм падає до мінімуму, який дозволяє плата (див. README).
//
// Три способи зберегти дані між Deep Sleep:
//   RTC_DATA_ATTR - RTC SRAM (8 КБ), швидко, без зносу, але зникає при знятті живлення;
//   NVS у flash   - переживає і відключення батареї, але повільно і з обмеженим ресурсом записів;
//   зовнішня памʼять (FRAM/EEPROM) - коли потрібно багато або надовго.
// PSRAM на N16R8 у Deep Sleep вимикається: туди дані «на після сну» не кладемо.

#include <inttypes.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "practice.h"

#define SLEEP_MS UINT64_C(5000)

static const char *TAG = "deep";

RTC_DATA_ATTR static uint32_t s_boot_count; // RTC SRAM: переживає Deep Sleep
static uint32_t s_ram_count;                // звичайна SRAM: обнуляється при кожному старті

void app_main(void) {
  s_boot_count++;
  s_ram_count++;

  // esp_reset_reason(): POWERON - перше вмикання, DEEPSLEEP - пробудження.
  const esp_reset_reason_t reason = esp_reset_reason();
  ESP_LOGI(TAG, "app_main() з початку. reset reason = %d (%s)", (int)reason,
           (reason == ESP_RST_DEEPSLEEP) ? "пробудження з Deep Sleep" : "не з Deep Sleep");
  ESP_LOGI(TAG, "RTC_DATA_ATTR s_boot_count = %" PRIu32 ", звичайна s_ram_count = %" PRIu32, s_boot_count,
           s_ram_count);

  // Тут була б корисна робота: виміряти, записати, відправити. Вона має бути короткою.

  ESP_LOGI(TAG, "Deep Sleep на %" PRIu64 " мс", SLEEP_MS);
  console_flush();
  ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(SLEEP_MS * 1000));
  esp_deep_sleep_start(); // не повертається: наступний рядок ніколи не виконається
}
