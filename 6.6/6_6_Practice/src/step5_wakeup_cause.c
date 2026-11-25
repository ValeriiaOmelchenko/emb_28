// Крок 5: «спочатку причина, потім дія». Той самий сон, що в кроці 4 (таймер + EXT0),
// але після старту програма розгалужується за причиною пробудження.
// Підключення: кнопка між GPIO4 і GND (див. README).
//
// У Deep Sleep кожне пробудження - це перезапуск, і app_main() не знає, чому вона
// стартувала, поки не спитає. Аналогія: телефон задзвонив - спершу дивишся, хто дзвонить
// (будильник чи начальник), і лише потім вирішуєш, що робити.
//
// Про API: у методичці switch (esp_sleep_get_wakeup_cause()). У ESP-IDF 6.x вона
// deprecated: якщо два джерела спрацюють майже одночасно, одне з них губиться.
// Нова esp_sleep_get_wakeup_causes() повертає бітову маску, тому перевіряємо кожен біт,
// а не switch по одному значенню.

#include <inttypes.h>
#include <stdint.h>

#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "practice.h"

#define SLEEP_MS UINT64_C(10000)

static const char *TAG = "cause";

RTC_DATA_ATTR static uint32_t s_timer_wakeups;
RTC_DATA_ATTR static uint32_t s_button_wakeups;

static void on_first_boot(void) {
  // Не з Deep Sleep: подали живлення, натиснули RESET, прошили або впав watchdog.
  // RTC SRAM після холодного старту обнуляється, лічильники починають з нуля.
  ESP_LOGI(TAG, "перший старт (reset reason = %d): ініціалізація", (int)esp_reset_reason());
}

static void on_timer(void) {
  s_timer_wakeups++;
  ESP_LOGI(TAG, "таймер: планове пробудження #%" PRIu32, s_timer_wakeups);
}

static void on_button(void) {
  s_button_wakeups++;
  ESP_LOGW(TAG, "кнопка: позапланова подія #%" PRIu32 " (таймерних було %" PRIu32 ")", s_button_wakeups,
           s_timer_wakeups);
}

static void deep_sleep_with_wakeups(bool button_wake) {
  ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(SLEEP_MS * 1000));
  if (button_wake) {
    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(BUTTON_PIN, BUTTON_PRESSED_LEVEL));
    ESP_ERROR_CHECK(rtc_gpio_pullup_en(BUTTON_PIN));
    ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(BUTTON_PIN));
  }
  ESP_LOGI(TAG, "Deep Sleep: таймер %" PRIu64 " мс%s", SLEEP_MS, button_wake ? " або кнопка GPIO4" : "");
  console_flush();
  esp_deep_sleep_start();
}

void app_main(void) {
  button_init();

  // 1. Причина. 2. Дія. Біти можуть бути встановлені одночасно - перевіряємо кожен.
  const uint32_t causes = esp_sleep_get_wakeup_causes();
  if (causes & BIT(ESP_SLEEP_WAKEUP_UNDEFINED)) {
    on_first_boot();
  }
  if (causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) {
    on_timer();
  }
  if (causes & BIT(ESP_SLEEP_WAKEUP_EXT0)) {
    on_button();
  }
  if ((causes & (BIT(ESP_SLEEP_WAKEUP_UNDEFINED) | BIT(ESP_SLEEP_WAKEUP_TIMER) | BIT(ESP_SLEEP_WAKEUP_EXT0))) == 0) {
    // Джерело, яке ми не вмикали: краще побачити це в лозі, ніж мовчки проігнорувати.
    ESP_LOGE(TAG, "неочікувана причина, маска 0x%08" PRIx32, causes);
  }

  const bool released = wait_button_released();
  if (!released) {
    ESP_LOGW(TAG, "кнопка затиснута > %" PRIu32 " мс - цей раз будить лише таймер", RELEASE_TIMEOUT_MS);
  }
  deep_sleep_with_wakeups(released);
}
