// Крок 4: Deep Sleep з ДВОМА джерелами пробудження: таймер (10 с) і кнопка через EXT0.
// Підключення: кнопка між GPIO4 і GND (див. README).
//
// EXT0 - один RTC GPIO, пробудження за рівнем. RTC-контролер живий у Deep Sleep
// і стежить за піном без CPU. Тому пін має бути саме RTC GPIO (ESP32-S3: GPIO 0-21).
// Підтяжка теж має бути RTC-шна (rtc_gpio_pullup_en): звичайна GPIO-підтяжка
// належить digital-домену, який у Deep Sleep знеструмлений.
//
// Тут лише друкуємо сиру маску причин. Розбір і реакцію - у кроці 5.

#include <inttypes.h>
#include <stdint.h>

#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "practice.h"

#define SLEEP_MS UINT64_C(10000)

static const char *TAG = "ext0";

RTC_DATA_ATTR static uint32_t s_boot_count;

static void deep_sleep_with_wakeups(bool button_wake) {
  ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(SLEEP_MS * 1000));
  if (button_wake) {
    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(BUTTON_PIN, BUTTON_PRESSED_LEVEL));
    // Неактивний рівень (1) тримає RTC-підтяжка, вона працює і уві сні.
    ESP_ERROR_CHECK(rtc_gpio_pullup_en(BUTTON_PIN));
    ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(BUTTON_PIN));
  }
  ESP_LOGI(TAG, "Deep Sleep: таймер %" PRIu64 " мс%s", SLEEP_MS, button_wake ? " або кнопка GPIO4" : "");
  console_flush();
  esp_deep_sleep_start();
}

void app_main(void) {
  s_boot_count++;
  button_init();

  // Бітова маска: BIT(ESP_SLEEP_WAKEUP_TIMER), BIT(ESP_SLEEP_WAKEUP_EXT0)...
  // Перше вмикання (не з Deep Sleep) - BIT(ESP_SLEEP_WAKEUP_UNDEFINED).
  const uint32_t causes = esp_sleep_get_wakeup_causes();
  ESP_LOGI(TAG, "старт #%" PRIu32 ", маска причин = 0x%08" PRIx32 " (TIMER = bit %d, EXT0 = bit %d)", s_boot_count,
           causes, ESP_SLEEP_WAKEUP_TIMER, ESP_SLEEP_WAKEUP_EXT0);

  // EXT0 будить за РІВНЕМ: якщо кнопку ще тримають, чип одразу прокинеться знову.
  // Залипла кнопка = вічний цикл перезавантажень і розряджена батарея.
  const bool released = wait_button_released();
  if (!released) {
    ESP_LOGW(TAG, "кнопка затиснута > %" PRIu32 " мс - цей раз будить лише таймер", RELEASE_TIMEOUT_MS);
  }
  deep_sleep_with_wakeups(released);
}
