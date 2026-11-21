// Крок 4: бінарний семафор ISR -> задача. Кнопка BOOT (GPIO0) на платі, нічого підключати не треба.
//
// Семафор - це СИГНАЛ, а не замок. У нього немає власника: дає (Give) один - переривання,
// бере (Take) інший - задача. Аналогія: дзвінок у двері. Хто натискає і хто відчиняє -
// різні люди. Mutex - ключ від замку: повернути його може лише той, хто взяв.
// Тому mutex з ISR не беруть (у переривання немає «власника», і чекати воно не може),
// а семафор з ISR дають - функцією з суфіксом FromISR.
//
// Що робить ISR (і нічого більше):
//   1. запамʼятовує час натискання (щоб виміряти затримку ISR -> задача);
//   2. вимикає своє переривання - це антибрязкіт: решта фронтів брязкоту просто не приходить;
//   3. xSemaphoreGiveFromISR - будить задачу;
//   4. portYIELD_FROM_ISR - якщо розбуджена задача важливіша за ту, яку перервали,
//      перемикнутись на неї одразу після виходу з ISR, а не на наступному тіку.
// Лог, обчислення, очікування - у задачі.
//
// USE_PORT_YIELD 0 - прибрати portYIELD_FROM_ISR і подивитись на затримку в лозі.
// Тік FreeRTOS у ESP-IDF за замовчуванням 10 мс (CONFIG_FREERTOS_HZ = 100), тому без yield
// затримка стрибає від одиниць мкс до ~10 мс: задача чекає, поки тік сам згадає про неї.

#include <inttypes.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "practice.h"

#define USE_PORT_YIELD 1

// BOOT на ESP32-S3-DevKitC-1: GPIO0 замикається на GND, на платі є зовнішній pull-up.
// GPIO0 - strapping-пін: якщо тримати кнопку під час ресету, плата увійде в режим прошивки.
// Після старту це звичайний вхід.
#define BUTTON_PIN            GPIO_NUM_0
#define BUTTON_PRESSED_LEVEL  0
#define BUTTON_RELEASED_LEVEL 1

#define BUTTON_WAIT_MS   UINT32_C(5000) // таймаут очікування: «задача жива, просто тихо»
#define DEBOUNCE_MS      UINT32_C(30)   // скільки «відпущено» має триматися стабільно
#define DEBOUNCE_POLL_MS UINT32_C(10)   // = 1 тік при CONFIG_FREERTOS_HZ = 100
#define PRIO_BUTTON      ((UBaseType_t)6)

static const char *TAG = "isr_sem";

static SemaphoreHandle_t s_button_sem;
static StaticSemaphore_t s_button_sem_buf;

// Час натискання, записаний в ISR. 64 біти на 32-бітному ядрі - це ДВА записи, і читання
// посередині дасть сміття. Тут це безпечно: задача читає значення лише ПІСЛЯ семафора,
// а переривання на цей час вимкнене - ISR не може переписати його вдруге.
static volatile int64_t s_press_time_us;

// IRAM_ATTR - код обробника в швидкій внутрішній RAM, а не у флеші (при записі у флеш
// кеш вимикається, і обробник у флеші не зміг би виконатись).
static void IRAM_ATTR button_isr(void *arg) {
  (void)arg;
  s_press_time_us = esp_timer_get_time();
  gpio_intr_disable(BUTTON_PIN); // увімкне задача, коли кнопку стабільно відпустять

  BaseType_t higher_prio_woken = pdFALSE;
  xSemaphoreGiveFromISR(s_button_sem, &higher_prio_woken); // НЕ xSemaphoreGive: в ISR це крах
#if USE_PORT_YIELD
  portYIELD_FROM_ISR(higher_prio_woken);
#else
  (void)higher_prio_woken;
#endif
}

// Чекає, поки кнопка буде відпущена стабільно DEBOUNCE_MS. Брязкіт скидає відлік.
// Задача при цьому спить між опитуваннями (vTaskDelay), CPU вільний.
static void wait_button_released(void) {
  uint32_t stable_ms = 0;
  while (stable_ms < DEBOUNCE_MS) {
    vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_POLL_MS));
    stable_ms = (gpio_get_level(BUTTON_PIN) == BUTTON_RELEASED_LEVEL) ? stable_ms + DEBOUNCE_POLL_MS : 0;
  }
}

static void button_task(void *arg) {
  (void)arg;
  uint32_t presses = 0;
  for (;;) {
    // Таймаут замість portMAX_DELAY: раз на 5 с задача гарантовано прокидається.
    // Тут можна годувати watchdog чи перевіряти, що кнопка не залипла.
    if (xSemaphoreTake(s_button_sem, pdMS_TO_TICKS(BUTTON_WAIT_MS)) != pdTRUE) {
      ESP_LOGI(TAG, "%" PRIu32 " мс без натискань, задача жива", BUTTON_WAIT_MS);
      continue;
    }
    const int64_t latency_us = esp_timer_get_time() - s_press_time_us;

    // Trust No Pin: поки переривання було вимкнене, пін міг «запамʼятати» фронт брязкоту,
    // і після ввімкнення ISR спрацює одразу. Якщо кнопка вже відпущена - це не натискання.
    if (gpio_get_level(BUTTON_PIN) != BUTTON_PRESSED_LEVEL) {
      ESP_LOGW(TAG, "фронт без натискання (завада або залишок брязкоту) - ігнорую");
    } else {
      presses++;
      ESP_LOGI(TAG, "натискання #%" PRIu32 ", ISR -> задача: %" PRId64 " мкс", presses, latency_us);
    }

    wait_button_released();
    gpio_intr_enable(BUTTON_PIN);
  }
}

void app_main(void) {
  // Порядок важливий: семафор -> задача -> переривання. Якщо ISR зареєструвати раніше,
  // натискання під час старту викличе xSemaphoreGiveFromISR(NULL) - і плата впаде.
  s_button_sem = xSemaphoreCreateBinaryStatic(&s_button_sem_buf);

  // Задача - на ядрі 0, там же, де встановлено обробник GPIO (gpio_install_isr_service
  // викликаємо з app_main, а він на ядрі 0). Тоді portYIELD_FROM_ISR перемикає саме це ядро.
  check_task_created(xTaskCreatePinnedToCore(button_task, "button", TASK_STACK_BYTES, NULL,
                                             PRIO_BUTTON, NULL, CORE_0),
                     "button");

  const gpio_config_t io_conf = {
      .pin_bit_mask = 1ULL << BUTTON_PIN,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE, // зовнішній pull-up на платі є, внутрішній - страховка
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_NEGEDGE, // натискання = перехід 1 -> 0
  };
  ESP_ERROR_CHECK(gpio_config(&io_conf));
  ESP_ERROR_CHECK(gpio_install_isr_service(0));
  ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON_PIN, button_isr, NULL));

  ESP_LOGI(TAG, "Натискай BOOT. USE_PORT_YIELD = %d", USE_PORT_YIELD);
}
