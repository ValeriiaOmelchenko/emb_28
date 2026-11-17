// Драйвер вбудованого RGB-світлодіода (WS2812) - спільний для кроків 2 і 3.
// Це «залізний» шар: автомат про RMT і WS2812 не знає, він лише каже rgb_led_set(колір).

#include "rgb_led.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "led_strip.h"

// Пін залежить від ревізії плати (надпис на звороті / шовкографія):
//   ESP32-S3-DevKitC-1 v1.0 - GPIO48, v1.1 - GPIO38.
// Якщо світлодіод не світиться - найперше перевірити це.
#define RGB_LED_PIN       GPIO_NUM_48
#define RGB_LED_COUNT     UINT32_C(1)
#define RMT_RESOLUTION_HZ UINT32_C(10000000) // 10 МГц: крок 0,1 мкс, достатньо для таймінгів WS2812

static led_strip_handle_t s_strip;

void rgb_led_init(void) {
  const led_strip_config_t strip_cfg = {
      .strip_gpio_num = RGB_LED_PIN,
      .max_leds = RGB_LED_COUNT,
      .led_model = LED_MODEL_WS2812,
      .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB, // WS2812 чекає G, R, B
  };
  const led_strip_rmt_config_t rmt_cfg = {
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .resolution_hz = RMT_RESOLUTION_HZ,
  };
  ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip));
  ESP_ERROR_CHECK(led_strip_clear(s_strip)); // після ресету WS2812 може памʼятати старий колір
}

void rgb_led_set(rgb_t color) {
  ESP_ERROR_CHECK(led_strip_set_pixel(s_strip, 0, color.r, color.g, color.b));
  ESP_ERROR_CHECK(led_strip_refresh(s_strip)); // set_pixel пише в буфер, refresh - відправляє
}
