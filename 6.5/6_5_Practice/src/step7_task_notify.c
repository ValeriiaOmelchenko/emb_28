// Крок 7: task notification ISR -> задача. Те саме, що крок 4, але без семафора.
// Кнопка BOOT (GPIO0) на платі, нічого підключати не треба.
//
// У кожної задачі FreeRTOS вже є вбудоване поле нотифікації: 32-бітне значення і стан
// «чекає / отримала». Тому окремий обʼєкт (StaticSemaphore_t) не потрібен: ISR пише
// прямо в TCB задачі. Аналогія: семафор - дзвінок у двері (хто почує, той відчинить),
// нотифікація - дзвінок на особистий мобільний конкретної людини.
//
// Заміна один до одного:
//   xSemaphoreGiveFromISR(sem, &woken)  ->  vTaskNotifyGiveFromISR(task, &woken)
//   xSemaphoreTake(sem, timeout)        ->  ulTaskNotifyTake(pdTRUE, timeout)
//
// ulTaskNotifyTake повертає значення лічильника ДО очищення:
//   pdTRUE  - обнулити після читання: поводиться як бінарний семафор (скільки б Give
//             не прийшло, задача прокинеться один раз і побачить їх кількість);
//   pdFALSE - відняти 1: поводиться як лічильний семафор.
// Тут переривання вимикається на першому ж фронті, тому значення завжди 1, а якщо ні -
// це видно в лозі.
//
// Обмеження (чому семафор не зник):
//   - отримувач лише ОДИН, і його handle треба знати заздалегідь;
//   - чекати нотифікацію може лише сама задача-власник;
//   - з ISR можна лише давати, не брати (як і із семафором).
//
// USE_PORT_YIELD 0 - прибрати portYIELD_FROM_ISR, як у кроці 4.

#include <inttypes.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "practice.h"

#define USE_PORT_YIELD 1

// BOOT на ESP32-S3-DevKitC-1: GPIO0 замикається на GND, на платі є зовнішній pull-up.
// GPIO0 - strapping-пін: якщо тримати кнопку під час ресету, плата увійде в режим прошивки.
#define BUTTON_PIN            GPIO_NUM_0
#define BUTTON_PRESSED_LEVEL  0
#define BUTTON_RELEASED_LEVEL 1

#define BUTTON_WAIT_MS   UINT32_C(5000) // таймаут очікування: «задача жива, просто тихо»
#define DEBOUNCE_MS      UINT32_C(30)   // скільки «відпущено» має триматися стабільно
#define DEBOUNCE_POLL_MS UINT32_C(10)   // = 1 тік при CONFIG_FREERTOS_HZ = 100
#define PRIO_BUTTON      ((UBaseType_t)6)

static const char *TAG = "notify";

// Замість SemaphoreHandle_t + StaticSemaphore_t - лише handle задачі-отримувача.
// Його пише app_main до реєстрації ISR, після цього тільки читають.
static TaskHandle_t s_button_task;

// Час натискання, записаний в ISR. Задача читає його лише після нотифікації,
// а переривання на цей час вимкнене (див. крок 4).
static volatile int64_t s_press_time_us;

static void IRAM_ATTR button_isr(void *arg) {
  (void)arg;
  s_press_time_us = esp_timer_get_time();
  gpio_intr_disable(BUTTON_PIN); // увімкне задача, коли кнопку стабільно відпустять

  BaseType_t higher_prio_woken = pdFALSE;
  vTaskNotifyGiveFromISR(s_button_task, &higher_prio_woken); // НЕ xTaskNotifyGive: в ISR це крах
#if USE_PORT_YIELD
  portYIELD_FROM_ISR(higher_prio_woken);
#else
  (void)higher_prio_woken;
#endif
}

// Чекає, поки кнопка буде відпущена стабільно DEBOUNCE_MS. Брязкіт скидає відлік.
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
    // 0 - таймаут, нотифікацій не було. Інакше - скільки Give прийшло з минулого разу.
    const uint32_t events = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BUTTON_WAIT_MS));
    if (events == 0) {
      ESP_LOGI(TAG, "%" PRIu32 " мс без натискань, задача жива", BUTTON_WAIT_MS);
      continue;
    }
    const int64_t latency_us = esp_timer_get_time() - s_press_time_us;

    // Trust No Pin: залишковий фронт брязкоту після ввімкнення переривання.
    if (gpio_get_level(BUTTON_PIN) != BUTTON_PRESSED_LEVEL) {
      ESP_LOGW(TAG, "фронт без натискання (завада або залишок брязкоту) - ігнорую");
    } else {
      presses++;
      ESP_LOGI(TAG, "натискання #%" PRIu32 ", нотифікацій: %" PRIu32 ", ISR -> задача: %" PRId64 " мкс",
               presses, events, latency_us);
    }

    wait_button_released();
    gpio_intr_enable(BUTTON_PIN);
  }
}

void app_main(void) {
  // Порядок: задача (handle) -> переривання. Якщо ISR зареєструвати раніше,
  // натискання під час старту викличе vTaskNotifyGiveFromISR(NULL) - і плата впаде.
  // Handle записується всередині xTaskCreate ще до того, як задача може стартувати.
  check_task_created(xTaskCreatePinnedToCore(button_task, "button", TASK_STACK_BYTES, NULL,
                                             PRIO_BUTTON, &s_button_task, CORE_0),
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

  // Скільки RAM заощадили: семафор з кроку 4 займав окремий обʼєкт такого розміру.
  ESP_LOGI(TAG, "Натискай BOOT. USE_PORT_YIELD = %d. StaticSemaphore_t з кроку 4: %u байт, тут: 0",
           USE_PORT_YIELD, (unsigned)sizeof(StaticSemaphore_t));
}
