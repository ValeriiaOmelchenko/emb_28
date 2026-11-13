// Крок 2: та сама задача з кроку 1, але без гонки - через чергу (xQueue).
//
// Ідея: у лічильника ОДИН власник - задача consumer. Продюсери на двох ядрах його
// не чіпають, а лише кладуть у чергу повідомлення «+1». Черга копіює дані і сама
// захищена всередині FreeRTOS, тому безпечна між задачами, ядрами і перериваннями.
// Аналогія: замість того щоб двоє касирів писали в один зошит, вони кидають чеки
// в скриньку, а бухгалтер (consumer) по одному виймає і записує.
//
// Друге джерело подій - переривання від кнопки BOOT (GPIO0). Антибрязкіт через
// вимкнення переривання:
//   1. Перший же спадний фронт -> ISR вимикає СВОЄ переривання, кладе подію в чергу
//      (xQueueSendToFrontFromISR) і запускає програмний таймер. Більше нічого:
//      ні вікон часу, ні рахування брязкоту. Брязкіт натискання ISR просто не бачить.
//   2. Таймер кожні 10 мс дивиться на пін. Коли кнопка стабільно відпущена 50 мс -
//      скидає «пропущені» фронти брязкоту і вмикає переривання назад.
// Аналогія: дзвінок у двері. Почув перший дзвінок - вимкнув дзвінок і пішов відчиняти.
// Увімкнув його назад, коли гість уже прибрав палець з кнопки.
//
// Порядок у app_main: черга, таймер -> consumer -> продюсери -> ISR -> старт.
// Переривання вмикаємо останнім: ISR, що спрацював раніше за xQueueCreateStatic, отримав би NULL.

#include <assert.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "hal/gpio_ll.h"
#include "practice.h"

// --- Залізо ---
// Кнопка BOOT на ESP32-S3-DevKitC-1 = GPIO0, замикає на GND (натиснута = 0).
// GPIO0 - strapping-пін: якщо тримати при подачі живлення/ресеті, плата піде в режим
// прошивки. Після старту його можна читати як звичайний вхід.
#define BUTTON_PIN GPIO_NUM_0
static_assert(BUTTON_PIN < 32, "скидання статусу нижче розраховане на GPIO 0-31");

// --- Параметри демо ---
#define ITERATIONS        UINT32_C(10000)          // повідомлень від кожного продюсера
#define QUEUE_LENGTH      ((UBaseType_t)32)
#define SEND_TIMEOUT      pdMS_TO_TICKS(100)       // скільки продюсер чекає місця в черзі
#define IDLE_TIMEOUT      pdMS_TO_TICKS(10000)     // тиша довше - пишемо «heartbeat» у лог
#define RELEASE_POLL_MS   UINT32_C(10)             // як часто таймер дивиться на кнопку
#define RELEASE_STABLE_MS UINT32_C(50)             // скільки кнопка має бути відпущена
#define RELEASE_SAMPLES   (RELEASE_STABLE_MS / RELEASE_POLL_MS)

static_assert(ITERATIONS <= UINT32_MAX / 2, "2 * ITERATIONS має влазити в uint32_t");
static_assert(QUEUE_LENGTH >= 4, "замала черга: продюсери постійно чекатимуть");
static_assert(RELEASE_STABLE_MS % RELEASE_POLL_MS == 0, "вікно має бути кратним періоду опитування");
static_assert(RELEASE_SAMPLES >= 2, "одне опитування - не антибрязкіт");

// --- Повідомлення черги ---
// Замість «магічних» uint8_t 1 і 100 - тип джерела і поле значення.
// Черга КОПІЮЄ повідомлення (8 байт), а не зберігає вказівник: відправник може
// одразу перевикористати свою змінну.
typedef enum {
  SRC_PRODUCER_A,
  SRC_PRODUCER_B,
  SRC_BUTTON,
  SRC_COUNT,
} event_source_t;

typedef struct {
  uint8_t  source;  // event_source_t; uint8_t - щоб повідомлення було компактним
  uint32_t value;   // продюсер: порядковий номер 0, 1, 2...; кнопка: тік натискання
} event_msg_t;

static const char *const SOURCE_NAMES[SRC_COUNT] = {
    [SRC_PRODUCER_A] = "prod_A",
    [SRC_PRODUCER_B] = "prod_B",
    [SRC_BUTTON]     = "button",
};

static const char *TAG = "step2_queue";

// і черга не може «не створитися» через фрагментацію або нестачу heap.
//   s_queue_storage - самі повідомлення: QUEUE_LENGTH x 8 байт = 256 байт;
//   s_queue_ctrl    - службова структура FreeRTOS (вказівники, списки задач, що чекають).
// Обидва буфери мають жити весь час роботи черги - тому static на рівні файлу,
// а НЕ локальні змінні app_main (стек app_main зникає разом з нею).
static uint8_t       s_queue_storage[QUEUE_LENGTH * sizeof(event_msg_t)];
static StaticQueue_t s_queue_ctrl;

static QueueHandle_t      s_queue;          // створюється ДО задач і ISR
static TimerHandle_t      s_release_timer;  // повертає переривання кнопки після відпускання

// Збої в ISR (повна черга подій або команд таймера). Пише лише ISR, читає consumer.
// _Atomic - щоб це було явно і коректно на двох ядрах.
static _Atomic uint32_t s_isr_errors;

// --- Продюсери ---
typedef struct {
  const char *name;
  BaseType_t  core;
  uint8_t     source;
} producer_config_t;

static const producer_config_t PRODUCERS[] = {
    {"prod_A", 0, SRC_PRODUCER_A},
    {"prod_B", 1, SRC_PRODUCER_B},
};
#define PRODUCER_COUNT (sizeof(PRODUCERS) / sizeof(PRODUCERS[0]))

static void producer_task(void *arg) {
  const producer_config_t *cfg = arg;
  ESP_LOGI(TAG, "%s на ядрі %d готовий", cfg->name, (int)xPortGetCoreID());

  uint32_t dropped = 0;
  for (uint32_t seq = 0; seq < ITERATIONS; ++seq) {
    const event_msg_t msg = {.source = cfg->source, .value = seq};
    // Обмежений таймаут замість portMAX_DELAY: якщо consumer завис, продюсер
    // не застрягне назавжди мовчки, а порахує втрати.
    if (xQueueSend(s_queue, &msg, SEND_TIMEOUT) != pdPASS) {
      ++dropped;
    }
  }

  ESP_LOGI(TAG, "%s: відправлено %" PRIu32 ", втрачено %" PRIu32 ", запас стеку %u Б",
           cfg->name, ITERATIONS - dropped, dropped,
           (unsigned)uxTaskGetStackHighWaterMark(NULL));
  vTaskDelete(NULL);  // задача FreeRTOS не може просто повернутися з функції
}

// --- ISR кнопки ---
// Правила ISR: мінімум роботи, без логів/printf/malloc, лише ...FromISR API.
// IRAM_ATTR - код обробника в IRAM: не чекає на кеш флешу, швидше стартує.
// Нюанс: сервіс gpio_install_isr_service(0) зареєстровано без ESP_INTR_FLAG_IRAM, тому
// під час запису у флеш переривання просто відкладається. Через це тут можна викликати
// gpio_intr_disable(), що лежить у флеші. З ESP_INTR_FLAG_IRAM усе, що викликає ISR,
// теж мало б бути в IRAM.
static void IRAM_ATTR button_isr(void *arg) {
  (void)arg;
  // 1. Перший же фронт: вимикаємо СВОЄ переривання. Брязкіт натискання, що йде далі,
  //    до процесора вже не дійде - ISR не потрібні ні вікна часу, ні лічильники фронтів.
  gpio_intr_disable(BUTTON_PIN);

  BaseType_t higher_prio_woken = pdFALSE;

  // 2. Хто ввімкне переривання назад - таймер після відпускання. Запускаємо його звідси,
  //    а не з consumer: навіть якщо подія в черзі загубиться, кнопка не «помре».
  if (xTimerStartFromISR(s_release_timer, &higher_prio_woken) != pdPASS) {
    // Черга команд таймерів повна. Краще пропустити антибрязкіт, ніж назавжди
    // лишитися без кнопки: вмикаємо переривання одразу і рахуємо збій.
    atomic_fetch_add(&s_isr_errors, 1);
    gpio_intr_enable(BUTTON_PIN);
  }

  // 3. Подія - consumer'у. Тривога - на ПОЧАТОК черги: її обробить раніше за накопичені «+1».
  const event_msg_t msg = {.source = SRC_BUTTON, .value = (uint32_t)xTaskGetTickCountFromISR()};
  if (xQueueSendToFrontFromISR(s_queue, &msg, &higher_prio_woken) != pdPASS) {
    atomic_fetch_add(&s_isr_errors, 1);  // черга повна: ISR чекати не може
  }

  // Якщо розбудили задачу з вищим пріоритетом - перемкнутись одразу після ISR.
  portYIELD_FROM_ISR(higher_prio_woken);
}

// --- Повернення переривання після відпускання ---
// Колбек програмного таймера працює в задачі таймерів FreeRTOS (не в ISR), тому тут
// можна спокійно читати пін. Блокуватись не можна: всі таймери ділять одну задачу.
static void release_timer_cb(TimerHandle_t timer) {
  // static: лічильник живе між викликами; чіпає його лише задача таймерів.
  static uint32_t s_released_samples;

  if (gpio_get_level(BUTTON_PIN) == 0) {
    s_released_samples = 0;  // ще тримають або брязкіт відпускання - починаємо відлік знову
    return;
  }
  if (++s_released_samples < RELEASE_SAMPLES) {
    return;
  }

  // Кнопка стабільно відпущена RELEASE_STABLE_MS.
  s_released_samples = 0;
  xTimerStop(timer, 0);  // 0 - не чекати: у колбеку таймера блокуватись не можна

  // Trust No Pin: поки переривання було вимкнене, фронти брязкоту при відпусканні
  // все одно записались у регістр статусу GPIO (вимикається лише доставка процесору).
  // Без скидання одразу після gpio_intr_enable() прилетіло б хибне «натискання».
  // У публічному драйвері такої функції немає, тому - напряму через HAL (hal/gpio_ll.h).
  gpio_ll_clear_intr_status(&GPIO, 1UL << BUTTON_PIN);
  gpio_intr_enable(BUTTON_PIN);
}

// --- Consumer: єдиний власник лічильників ---
static void consumer_task(void *arg) {
  (void)arg;
  // Локальні змінні: їх бачить лише ця задача - гонки немає за побудовою.
  uint32_t counter = 0;
  uint32_t next_seq[SRC_COUNT] = {0};
  uint32_t presses = 0;
  bool summary_printed = false;

  for (;;) {
    event_msg_t msg;
    // Таймаут, а не portMAX_DELAY: якщо всі джерела замовкли, це видно в лозі.
    if (xQueueReceive(s_queue, &msg, IDLE_TIMEOUT) != pdPASS) {
      ESP_LOGI(TAG, "heartbeat: лічильник %" PRIu32 ", натискань %" PRIu32
               ", збоїв у ISR %" PRIu32 ", запас стеку %u Б",
               counter, presses, atomic_load(&s_isr_errors),
               (unsigned)uxTaskGetStackHighWaterMark(NULL));
      continue;
    }

    switch (msg.source) {
      case SRC_PRODUCER_A:
      case SRC_PRODUCER_B:
        // Черга - FIFO: від одного продюсера номери мають іти підряд.
        if (msg.value != next_seq[msg.source]) {
          ESP_LOGE(TAG, "%s: чекали #%" PRIu32 ", прийшов #%" PRIu32,
                   SOURCE_NAMES[msg.source], next_seq[msg.source], msg.value);
        }
        next_seq[msg.source] = msg.value + 1;
        ++counter;
        if (!summary_printed && counter == 2 * ITERATIONS) {
          summary_printed = true;
          ESP_LOGI(TAG, "Лічильник: %" PRIu32 " з %" PRIu32 " - жодного втраченого інкременту",
                   counter, 2 * ITERATIONS);
          ESP_LOGI(TAG, "Тепер натисни BOOT");
        }
        break;

      case SRC_BUTTON:
        ++presses;
        ESP_LOGW(TAG, "BOOT #%" PRIu32 " (t=%" PRIu32 " мс), лічильник у цей момент: %" PRIu32,
                 presses, (uint32_t)pdTICKS_TO_MS(msg.value), counter);
        break;

      default:
        // Невідоме джерело - баг відправника. Не мовчимо.
        ESP_LOGE(TAG, "Невідоме джерело %u", (unsigned)msg.source);
        break;
    }
  }
}

static void button_init(void) {
  const gpio_config_t io_conf = {
      .pin_bit_mask = 1ULL << BUTTON_PIN,
      .mode = GPIO_MODE_INPUT,
      // Trust No Pin: вмикаємо внутрішню підтяжку явно, а не покладаємось на плату.
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_NEGEDGE,  // натискання = спадний фронт; далі ISR вимкне себе
  };
  ESP_ERROR_CHECK(gpio_config(&io_conf));
  ESP_ERROR_CHECK(gpio_install_isr_service(0));
  ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON_PIN, button_isr, NULL));
}

void app_main(void) {
  ESP_LOGI(TAG, "=== Черга: 2 продюсери x %" PRIu32 " повідомлень + ISR кнопки BOOT ===",
           ITERATIONS);

  // 1. Обʼєкти FreeRTOS - першими. Черга - у статичних буферах (heap не чіпаємо);
  //    таймер - з heap один раз на старті, не в циклі.
  //    Порівняй: xQueueCreate(QUEUE_LENGTH, sizeof(event_msg_t)) - те саме, але з heap.
  s_queue = xQueueCreateStatic(QUEUE_LENGTH, sizeof(event_msg_t), s_queue_storage, &s_queue_ctrl);
  // Таймер антибрязкоту: автоперезапуск кожні RELEASE_POLL_MS, поки кнопку не відпустять.
  s_release_timer = xTimerCreate("btn_release", pdMS_TO_TICKS(RELEASE_POLL_MS), pdTRUE, NULL,
                                 release_timer_cb);
  if (s_queue == NULL || s_release_timer == NULL) {
    ESP_LOGE(TAG, "Не вдалося створити чергу / таймер");
    abort();
  }

  // Як у кроці 1: на час створення app_main важливіша за всіх, інакше перший же продюсер
  // на ядрі 0 витіснить її і встигне відправити все до того, як створимо другого.
  const UBaseType_t main_prio = uxTaskPriorityGet(NULL);
  vTaskPrioritySet(NULL, PRIO_INIT);

  // 2. Consumer з вищим пріоритетом (10 > 5): щойно в черзі щось є, він розбирає це
  //    раніше за продюсерів, черга не переповнюється, кнопка обробляється одразу.
  //    Без прив'язки до ядра - планувальник SMP поставить на вільне.
  check_task_created(xTaskCreate(consumer_task, "consumer", LOG_TASK_STACK_BYTES, NULL,
                                 PRIO_CONSUMER, NULL),
                     "consumer");

  // 3. Продюсери - по одному на кожне ядро, як у кроці 1.
  for (size_t i = 0; i < PRODUCER_COUNT; ++i) {
    check_task_created(xTaskCreatePinnedToCore(producer_task, PRODUCERS[i].name,
                                               LOG_TASK_STACK_BYTES, (void *)&PRODUCERS[i],
                                               PRIO_WORKER, NULL, PRODUCERS[i].core),
                       PRODUCERS[i].name);
  }

  // 4. Переривання - останнім: черга і таймер уже існують.
  button_init();

  // 5. Старт: повертаємо app_main пріоритет 1, і всі задачі починають працювати разом.
  ESP_LOGI(TAG, "=== Старт ===");
  vTaskPrioritySet(NULL, main_prio);
}
