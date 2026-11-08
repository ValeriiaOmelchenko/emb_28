// Крок 2. volatile: без нього цикл не бачить зміну, зроблену в ISR.
//
// ISR кнопки BOOT ставить ДВА прапори: звичайний і volatile.
// app_main() чекає на одному з них (USE_VOLATILE).
//   false → натискаєш BOOT, а програма «зависла»: компілятор прочитав plainFlag один раз
//           до циклу і крутить нескінченний цикл з копією в регістрі.
//   true  → кожна перевірка читає змінну з пам'яті, натискання видно.
//
// Важливо: ефект видно лише з оптимізацією (-Os у sdkconfig.defaults). У типовій для ESP-IDF
// debug-збірці (-Og) компілятор перечитує змінну, і програма «працює» без volatile. Баг ховається.
//
// Бонус: busy-wait у задачі main (ядро 0) не дає працювати задачі IDLE0, і раз на 5 с
// Task Watchdog друкує «task_wdt: Task watchdog got triggered». Це не баг демонстрації,
// а нагадування, чому в реальному коді так не чекають.

#if PRACTICE_STEP == 2

#include <cstdio>

#include "driver/gpio.h"
#include "esp_attr.h"

constexpr gpio_num_t PIN_BOOT = GPIO_NUM_0;  // кнопка BOOT на DevKitC-1: замикає GPIO 0 на GND

constexpr bool USE_VOLATILE = false;

static bool plainFlag = false;
static volatile bool volatileFlag = false;

// IRAM_ATTR: код ISR лежить у RAM, бо під час запису у Flash кеш вимикається.
static void IRAM_ATTR onBootPressed(void*) {
  plainFlag = true;
  volatileFlag = true;
}

static void initBootButton(gpio_isr_t handler) {
  const gpio_config_t config = {
      .pin_bit_mask = 1ULL << PIN_BOOT,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,  // на платі вже є 10 кОм, внутрішня - про запас
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_NEGEDGE,
  };
  ESP_ERROR_CHECK(gpio_config(&config));
  ESP_ERROR_CHECK(gpio_install_isr_service(0));
  ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_BOOT, handler, nullptr));
}

extern "C" void app_main() {
  initBootButton(onBootPressed);
  printf("\n=== Крок 2: чекаю на %s прапор. Натискай BOOT ===\n",
         USE_VOLATILE ? "volatile" : "звичайний");

  while (true) {
    // Навмисний busy-wait: у порожньому циклі немає викликів функцій,
    // тож компілятор має право не перечитувати звичайну змінну.
    if (USE_VOLATILE) {
      while (!volatileFlag) {
      }
      volatileFlag = false;
    } else {
      while (!plainFlag) {
      }
      plainFlag = false;
    }

    printf("Побачила натискання BOOT\n");
  }
}

#endif  // PRACTICE_STEP == 2
