// Еталон 6.3 - FreeRTOS: черги.
// ESP32-S3-DevKitC-1, ESP-IDF, чистий C17.
//
// Кілька датчиків -> одна черга -> один consumer:
//   temp     (producer) - внутрішній датчик температури чипа, строго кожні 1000 мс;
//   pot      (producer) - потенціометр на GPIO1 (ADC1), 16 вимірювань усереднено, кожні 250 мс;
//   consumer            - єдиний, хто бачить дані: друкує, рахує втрати, помічає тишу.
//
// Спільних змінних між задачами немає. У даних один власник (consumer), продюсери передають
// йому КОПІЇ повідомлень через xQueue. Черга сама захищена всередині FreeRTOS, тому безпечна
// між задачами на обох ядрах S3 (і з ISR - через ...FromISR).
// Аналогія: датчики кидають листи в поштову скриньку, а диспетчер виймає їх по одному.

#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "driver/temperature_sensor.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "ref_6_3";

// --- Залізо ---
// Потенціометр 10 кОм: крайні виводи на 3V3 і GND, повзунок на GPIO1 (ADC1_CH0).
// ADC1 (GPIO 1-10) працює разом з Wi-Fi, ADC2 (GPIO 11-20) - ні.
// Атенюація 12 дБ: діапазон до ~3,1 В, верхні ~0,2 В потенціометра АЦП не розрізняє.
#define POT_ADC_UNIT    ADC_UNIT_1
#define POT_ADC_CHANNEL ADC_CHANNEL_0
#define POT_ADC_ATTEN   ADC_ATTEN_DB_12
// Trust No Pin: одне вимірювання АЦП шумить на десятки одиниць. Середнє з 16 зменшує шум
// приблизно в 4 рази (корінь з N). 16 x 4095 легко влазить в int32_t.
#define POT_SAMPLES     UINT8_C(16)

// Внутрішній датчик міряє температуру КРИСТАЛА (на кілька градусів вища за кімнатну).
// Діапазон -10...80 °C - під нього драйвер вибирає найточніший режим вимірювання.
#define TSENS_MIN_C (-10)
#define TSENS_MAX_C 80

// --- Час ---
#define TEMP_PERIOD_MS  UINT32_C(1000)
#define POT_PERIOD_MS   UINT32_C(250)
// Скільки продюсер чекає місця в черзі. Набагато менше за період: завислий consumer не має
// зупиняти опитування датчика, а втрата має бути видна в лозі (а не portMAX_DELAY).
#define SEND_TIMEOUT_MS UINT32_C(10)
// Датчик, що мовчить довше за 3 своїх періоди, вважаємо зниклим: одна-дві пропущені точки -
// ще не тривога, три - вже так.
#define SILENCE_PERIODS UINT32_C(3)
// Як довго consumer чекає повідомлення, перш ніж перевірити тишу (якщо замовкли ВСІ датчики).
#define RX_TIMEOUT_MS   UINT32_C(500)
#define STATS_PERIOD_MS UINT32_C(10000)

static_assert(SEND_TIMEOUT_MS < POT_PERIOD_MS, "таймаут відправки має бути меншим за період");
static_assert(RX_TIMEOUT_MS < SILENCE_PERIODS * POT_PERIOD_MS,
              "consumer має перевіряти тишу частіше, ніж спливає найкоротше вікно");
static_assert(pdMS_TO_TICKS(SEND_TIMEOUT_MS) > 0, "таймаут коротший за тік FreeRTOS");

// --- Черга ---
// У нормі consumer (важливіший) забирає повідомлення одразу, і в черзі 0-1 елемент.
// 8 місць - запас на випадок, коли consumer на мить зайнятий логом.
// Довша черга втрат не лікує: якщо consumer повільніший за датчики, вона лише зберігає
// старіші дані довше, а потім однаково переповнюється.
#define QUEUE_LENGTH ((UBaseType_t)8)

// --- Задачі ---
// В ESP-IDF стек задачі - у БАЙТАХ. ESP_LOGx зʼїдає 1,5-2 КБ, тому 2048 для задачі з логом - впритул.
#define TASK_STACK_BYTES UINT32_C(3072)
// Consumer важливіший за продюсерів: щойно щось лягло в чергу, він це забирає.
#define PRIO_PRODUCER ((UBaseType_t)(tskIDLE_PRIORITY + 2))
#define PRIO_CONSUMER ((UBaseType_t)(tskIDLE_PRIORITY + 3))
static_assert(PRIO_PRODUCER < PRIO_CONSUMER, "consumer має бути важливішим за продюсерів");

// ---------------------------------------------------------------------------
// Повідомлення
// ---------------------------------------------------------------------------
typedef enum {
  SENSOR_TEMP,
  SENSOR_POT,
  SENSOR_COUNT,
} sensor_id_t;

// Черга КОПІЮЄ повідомлення (16 байт), а не зберігає вказівник: продюсер може одразу
// перевикористати свою змінну. Тому і значення - ціле число у фіксованих одиницях
// (fixed-point), а не вказівник чи float.
typedef struct {
  sensor_id_t source;   // «зворотна адреса»: без неї consumer не знає, чиє це число
  uint32_t    seq;      // порядковий номер від цього датчика: розрив = втрачені повідомлення
  uint32_t    time_ms;  // коли виміряно (а не коли дійшло до consumer)
  int32_t     value;    // temp: сотні частки °C (2431 = 24,31 °C); pot: мВ
} sensor_msg_t;
static_assert(sizeof(sensor_msg_t) == 16, "повідомлення копіюється в чергу цілим - тримаємо його малим");

// ---------------------------------------------------------------------------
// Драйвери датчиків
// ---------------------------------------------------------------------------
static temperature_sensor_handle_t s_tsens;
static adc_oneshot_unit_handle_t   s_adc;
static adc_cali_handle_t           s_adc_cali;

static esp_err_t read_temp(int32_t *out_centi_c) {
  float celsius = 0.0f;
  const esp_err_t err = temperature_sensor_get_celsius(s_tsens, &celsius);
  if (err == ESP_OK) {
    *out_centi_c = (int32_t)lroundf(celsius * 100.0f);
  }
  return err;
}

static esp_err_t read_pot(int32_t *out_mv) {
  int32_t sum = 0;
  for (uint8_t i = 0; i < POT_SAMPLES; ++i) {
    int raw = 0;
    const esp_err_t err = adc_oneshot_read(s_adc, POT_ADC_CHANNEL, &raw);
    if (err != ESP_OK) {
      return err;
    }
    sum += raw;
  }
  // Калібрування (криві з eFuse конкретного чипа) переводить сирі одиниці в мВ: залежність
  // АЦП ESP32 нелінійна, і «raw * 3300 / 4095» на краях помиляється на сотні мВ.
  int mv = 0;
  const esp_err_t err = adc_cali_raw_to_voltage(s_adc_cali, (int)(sum / POT_SAMPLES), &mv);
  if (err == ESP_OK) {
    *out_mv = mv;
  }
  return err;
}

static void sensors_init(void) {
  const temperature_sensor_config_t tsens_conf =
      TEMPERATURE_SENSOR_CONFIG_DEFAULT(TSENS_MIN_C, TSENS_MAX_C);
  ESP_ERROR_CHECK(temperature_sensor_install(&tsens_conf, &s_tsens));
  ESP_ERROR_CHECK(temperature_sensor_enable(s_tsens));

  const adc_oneshot_unit_init_cfg_t unit_conf = {.unit_id = POT_ADC_UNIT};
  ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_conf, &s_adc));
  const adc_oneshot_chan_cfg_t chan_conf = {
      .atten    = POT_ADC_ATTEN,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
  };
  ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, POT_ADC_CHANNEL, &chan_conf));

  const adc_cali_curve_fitting_config_t cali_conf = {
      .unit_id  = POT_ADC_UNIT,
      .chan     = POT_ADC_CHANNEL,
      .atten    = POT_ADC_ATTEN,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
  };
  ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cali_conf, &s_adc_cali));
}

// ---------------------------------------------------------------------------
// Таблиця датчиків: одна функція продюсера на всіх
// ---------------------------------------------------------------------------
typedef esp_err_t (*sensor_read_fn)(int32_t *out);

typedef struct {
  sensor_id_t    id;
  const char    *name;       // і імʼя задачі: не довше 15 символів, інакше обріжеться
  uint32_t       period_ms;
  sensor_read_fn read;
} sensor_cfg_t;

// Індекс = id (designated initializers): SENSORS[msg.source] завжди відповідає джерелу.
// static const -> лежить у флеші.
static const sensor_cfg_t SENSORS[SENSOR_COUNT] = {
    [SENSOR_TEMP] = {SENSOR_TEMP, "temp", TEMP_PERIOD_MS, read_temp},
    [SENSOR_POT]  = {SENSOR_POT,  "pot",  POT_PERIOD_MS,  read_pot},
};

// ---------------------------------------------------------------------------
// Черга - у статичних буферах: heap не потрібен, розмір видно у звіті RAM при збірці,
// і черга не може «не створитися» через нестачу heap. Буфери мають жити, поки живе черга, -
// тому static на рівні файлу, а не локальні змінні app_main.
// ---------------------------------------------------------------------------
static uint8_t       s_queue_storage[QUEUE_LENGTH * sizeof(sensor_msg_t)];
static StaticQueue_t s_queue_ctrl;
static QueueHandle_t s_queue;  // створюється ПЕРШИМ, до датчиків і задач

// ---------------------------------------------------------------------------
// Producer
// ---------------------------------------------------------------------------
static void producer_task(void *arg) {
  const sensor_cfg_t *cfg = arg;  // void * -> вказівник неявно, const не губимо
  const TickType_t period = pdMS_TO_TICKS(cfg->period_ms);
  TickType_t last_wake = xTaskGetTickCount();
  uint32_t seq = 0;
  uint32_t dropped = 0;

  for (;;) {
    // Строга періодичність: наступне пробудження від запланованого моменту, а не від «зараз».
    xTaskDelayUntil(&last_wake, period);

    int32_t value = 0;
    const esp_err_t err = cfg->read(&value);
    if (err != ESP_OK) {
      // Помилка - не значення: у чергу її як число не кладемо.
      ESP_LOGW(TAG, "%s: помилка читання: %s", cfg->name, esp_err_to_name(err));
      continue;
    }

    const sensor_msg_t msg = {
        .source  = cfg->id,
        .seq     = seq++,  // росте і тоді, коли відправка не вдалась: consumer побачить розрив
        .time_ms = (uint32_t)pdTICKS_TO_MS(last_wake),
        .value   = value,
    };
    if (xQueueSend(s_queue, &msg, pdMS_TO_TICKS(SEND_TIMEOUT_MS)) != pdPASS) {
      // Черга повна (consumer завис або не встигає). Датчик не блокуємо, а рахуємо втрату.
      ++dropped;
      ESP_LOGW(TAG, "%s: черга повна, вимірювання #%" PRIu32 " втрачено (усього %" PRIu32 ")",
               cfg->name, msg.seq, dropped);
    }
  }
}

// ---------------------------------------------------------------------------
// Consumer: єдиний власник статистики
// ---------------------------------------------------------------------------
typedef struct {
  uint32_t   received;
  uint32_t   lost;          // за розривами в seq
  uint32_t   next_seq;
  TickType_t last_rx_tick;
  bool       silent;        // уже попередили про тишу - не спамимо щоразу
} source_stats_t;

static void handle_message(source_stats_t stats[], const sensor_msg_t *msg) {
  // Повідомлення - дані ззовні задачі. Перш ніж брати stats[msg->source], перевіряємо межі:
  // пошкоджене або нове джерело не має писати за межі масиву.
  if ((unsigned)msg->source >= SENSOR_COUNT) {
    ESP_LOGE(TAG, "невідоме джерело %u - повідомлення відкинуто", (unsigned)msg->source);
    return;
  }
  source_stats_t *st = &stats[msg->source];
  const char *name = SENSORS[msg->source].name;

  // Черга - FIFO, тож від одного датчика номери мають іти підряд. Розрив = стільки вимірювань
  // загубилось дорогою (продюсер не зміг покласти їх у повну чергу).
  if (msg->seq > st->next_seq) {
    const uint32_t gap = msg->seq - st->next_seq;
    st->lost += gap;
    ESP_LOGW(TAG, "%s: пропущено %" PRIu32 " вимірювань перед #%" PRIu32, name, gap, msg->seq);
  }
  st->next_seq = msg->seq + 1;
  ++st->received;
  st->last_rx_tick = xTaskGetTickCount();
  if (st->silent) {
    st->silent = false;
    ESP_LOGI(TAG, "%s: знову на звʼязку", name);
  }

  switch (msg->source) {
    case SENSOR_TEMP: {
      // Fixed-point -> текст без float: цілі градуси і сотні частки окремо.
      const int32_t v = msg->value;
      const uint32_t abs_v = (uint32_t)(v < 0 ? -v : v);
      ESP_LOGI(TAG, "%-4s #%-4" PRIu32 " t=%6" PRIu32 " ms  %s%" PRIu32 ".%02" PRIu32 " C", name,
               msg->seq, msg->time_ms, v < 0 ? "-" : "", abs_v / 100U, abs_v % 100U);
      break;
    }
    case SENSOR_POT:
      ESP_LOGI(TAG, "%-4s #%-4" PRIu32 " t=%6" PRIu32 " ms  %" PRId32 " mV", name, msg->seq,
               msg->time_ms, msg->value);
      break;
    default:
      // Новий датчик додали в enum, а тут забули: не мовчимо.
      ESP_LOGE(TAG, "%s: немає обробника", name);
      break;
  }
}

// Тиша перевіряється ПО КОЖНОМУ датчику: якщо один продюсер не стартував або завис, а інший
// працює, загальний таймаут xQueueReceive цього не помітить - повідомлення ж ідуть.
static void check_silence(source_stats_t stats[]) {
  const TickType_t now = xTaskGetTickCount();
  for (size_t i = 0; i < SENSOR_COUNT; ++i) {
    const TickType_t window = pdMS_TO_TICKS(SILENCE_PERIODS * SENSORS[i].period_ms);
    // Беззнакова різниця тіків коректна і при переповненні лічильника.
    if (!stats[i].silent && (TickType_t)(now - stats[i].last_rx_tick) >= window) {
      stats[i].silent = true;
      ESP_LOGW(TAG, "%s: немає даних понад %" PRIu32 " ms", SENSORS[i].name,
               SILENCE_PERIODS * SENSORS[i].period_ms);
    }
  }
}

static void print_stats(const source_stats_t stats[], UBaseType_t peak_depth) {
  for (size_t i = 0; i < SENSOR_COUNT; ++i) {
    ESP_LOGI(TAG, "stats %-4s: отримано %" PRIu32 ", втрачено %" PRIu32, SENSORS[i].name,
             stats[i].received, stats[i].lost);
  }
  ESP_LOGI(TAG, "stats queue: пік %u з %u", (unsigned)peak_depth, (unsigned)QUEUE_LENGTH);
}

static void consumer_task(void *arg) {
  (void)arg;
  // Локальні змінні: їх бачить лише ця задача - гонки немає за побудовою.
  source_stats_t stats[SENSOR_COUNT] = {0};
  const TickType_t start = xTaskGetTickCount();
  for (size_t i = 0; i < SENSOR_COUNT; ++i) {
    stats[i].last_rx_tick = start;  // відлік тиші - від старту, а не від нуля
  }
  UBaseType_t peak_depth = 0;
  TickType_t last_stats = start;

  for (;;) {
    sensor_msg_t msg;
    // Таймаут, а не portMAX_DELAY: якщо замовкнуть усі датчики, consumer це помітить.
    if (xQueueReceive(s_queue, &msg, pdMS_TO_TICKS(RX_TIMEOUT_MS)) == pdPASS) {
      // Скільки було в черзі разом із щойно взятим: так видно, чи consumer встигає.
      const UBaseType_t depth = uxQueueMessagesWaiting(s_queue) + 1;
      if (depth > peak_depth) {
        peak_depth = depth;
      }
      handle_message(stats, &msg);
    }

    check_silence(stats);

    if ((TickType_t)(xTaskGetTickCount() - last_stats) >= pdMS_TO_TICKS(STATS_PERIOD_MS)) {
      last_stats += pdMS_TO_TICKS(STATS_PERIOD_MS);
      print_stats(stats, peak_depth);
    }
  }
}

// ---------------------------------------------------------------------------
// Старт
// ---------------------------------------------------------------------------

// Кожна задача перевіряється окремо через != pdPASS.
// Пастка: ok &= xTaskCreate(...) не ловить помилку, бо pdPASS = 1, а помилка = -1
// (усі біти одиниці), і 1 & -1 == 1. Перевірка, після якої програма йде далі, - не перевірка.
static void create_task_or_abort(TaskFunction_t fn, const char *name, void *arg,
                                 UBaseType_t prio) {
  if (xTaskCreate(fn, name, TASK_STACK_BYTES, arg, prio, NULL) != pdPASS) {
    ESP_LOGE(TAG, "задачу %s не створено (мало heap?)", name);
    abort();
  }
}

void app_main(void) {
  ESP_LOGI(TAG, "=== 6.3: %u датчики -> черга на %u -> consumer ===", (unsigned)SENSOR_COUNT,
           (unsigned)QUEUE_LENGTH);

  // Порядок: черга -> периферія -> consumer -> продюсери (-> переривання, якби були, - останніми).
  // Кожен наступний крок спирається на попередній: продюсер, що стартував раніше за чергу,
  // відправив би в NULL; датчик, опитаний до ініціалізації драйвера, «не знайдено».

  // 1. Черга. Зі статичними буферами не може повернути NULL, але configASSERT дешевий.
  s_queue = xQueueCreateStatic(QUEUE_LENGTH, sizeof(sensor_msg_t), s_queue_storage, &s_queue_ctrl);
  configASSERT(s_queue != NULL);

  // 2. Периферія. ESP_ERROR_CHECK зупинить програму з назвою помилки, а не піде далі.
  sensors_init();

  // 3. Consumer - до продюсерів: перше ж повідомлення вже є кому забрати.
  create_task_or_abort(consumer_task, "consumer", NULL, PRIO_CONSUMER);

  // 4. Продюсери - з таблиці, одна функція на всіх. Без прив'язки до ядра: черга безпечна
  //    між ядрами, планувальник SMP поставить задачі на вільне.
  for (size_t i = 0; i < SENSOR_COUNT; ++i) {
    configASSERT(SENSORS[i].id == (sensor_id_t)i && SENSORS[i].read != NULL);
    create_task_or_abort(producer_task, SENSORS[i].name, (void *)&SENSORS[i], PRIO_PRODUCER);
  }
  // app_main може повернутись: задачі живуть далі.
}
