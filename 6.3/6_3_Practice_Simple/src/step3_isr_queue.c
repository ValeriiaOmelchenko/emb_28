// Крок 3: подія з переривання -> черга -> задача.
//
// ISR кнопки BOOT (GPIO0) нічого не обробляє сам: кладе в чергу момент натискання,
// а задача вже логує і робить усе «повільне».
//
// Антибрязкіт через вимкнення переривання:
//   1. Перший спадний фронт -> ISR вимикає СВОЄ переривання і кладе подію в чергу.
//      Брязкіт натискання ISR більше не бачить: ні вікон часу, ні рахування фронтів.
//   2. Задача після обробки чекає, поки кнопка 50 мс стабільно відпущена,
//      і лише тоді вмикає переривання назад.
// Аналогія: почув дзвінок у двері - вимкнув дзвінок і пішов відчиняти.
// Увімкнув його назад, коли гість прибрав палець з кнопки.

#include <inttypes.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hal/gpio_ll.h"
#include "practice.h"

// Кнопка BOOT на ESP32-S3-DevKitC-1 = GPIO0, замикає на GND (натиснута = 0).
// GPIO0 - strapping-пін: не тримай кнопку під час ресету, інакше плата піде в режим прошивки.
#define BUTTON_PIN      GPIO_NUM_0
#define QUEUE_LENGTH    ((UBaseType_t)4)
#define RELEASE_POLL    pdMS_TO_TICKS(10)  // як часто дивимось на кнопку після натискання
#define RELEASE_SAMPLES 5                  // 5 x 10 мс = 50 мс стабільно відпущена
#define PRIO_BUTTON     ((UBaseType_t)2)

static const char *TAG = "step3_isr";

static QueueHandle_t s_queue;

// Правила ISR: мінімум роботи, без логів/printf/malloc, лише ...FromISR API.
// IRAM_ATTR - код обробника в IRAM, не чекає на кеш флешу.
static void IRAM_ATTR button_isr(void *arg) {
  (void)arg;
  gpio_intr_disable(BUTTON_PIN);  // брязкіт далі до процесора не дійде

  const TickType_t pressed_at = xTaskGetTickCountFromISR();
  BaseType_t higher_prio_woken = pdFALSE;
  // ISR чекати не може. Але поки переривання вимкнене, в черзі максимум одна подія,
  // тож місце в ній є завжди.
  xQueueSendFromISR(s_queue, &pressed_at, &higher_prio_woken);
  // Розбудили задачу з вищим пріоритетом - перемкнутися на неї одразу після ISR.
  portYIELD_FROM_ISR(higher_prio_woken);
}

// Чекаємо, поки кнопка RELEASE_SAMPLES опитувань поспіль у стані «відпущено».
// Брязкіт відпускання скидає відлік.
static void wait_until_released(void) {
  uint8_t stable = 0;
  while (stable < RELEASE_SAMPLES) {
    vTaskDelay(RELEASE_POLL);
    stable = (gpio_get_level(BUTTON_PIN) == 1) ? (uint8_t)(stable + 1) : 0;
  }
}

static void button_task(void *arg) {
  (void)arg;
  uint32_t presses = 0;
  for (;;) {
    TickType_t pressed_at;
    // portMAX_DELAY тут доречний: чекаємо людину, а не датчик, що мав би щось слати.
    xQueueReceive(s_queue, &pressed_at, portMAX_DELAY);
    ++presses;
    ESP_LOGI(TAG, "BOOT #%" PRIu32 " (t=%" PRIu32 " мс)", presses,
             (uint32_t)pdTICKS_TO_MS(pressed_at));

    wait_until_released();
    // Trust No Pin: поки переривання було вимкнене, фронти брязкоту все одно записались
    // у регістр статусу GPIO. Без скидання одразу прилетіло б хибне «натискання».
    gpio_ll_clear_intr_status(&GPIO, 1UL << BUTTON_PIN);
    gpio_intr_enable(BUTTON_PIN);
  }
}

void app_main(void) {
  ESP_LOGI(TAG, "=== ISR -> черга: натискай BOOT ===");

  // Порядок: черга -> задача -> переривання. ISR, що спрацював раніше за xQueueCreate,
  // отримав би NULL і assert.
  s_queue = xQueueCreate(QUEUE_LENGTH, sizeof(TickType_t));
  if (s_queue == NULL) {
    ESP_LOGE(TAG, "Не вдалося створити чергу");
    abort();
  }
  check_task_created(
      xTaskCreate(button_task, "button", TASK_STACK_BYTES, NULL, PRIO_BUTTON, NULL), "button");

  const gpio_config_t io_conf = {
      .pin_bit_mask = 1ULL << BUTTON_PIN,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,  // Trust No Pin: підтяжку вмикаємо явно
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_NEGEDGE,    // натискання = спадний фронт
  };
  ESP_ERROR_CHECK(gpio_config(&io_conf));
  ESP_ERROR_CHECK(gpio_install_isr_service(0));
  ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON_PIN, button_isr, NULL));
}
