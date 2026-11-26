// Крок 5: antipattern «malloc/free у циклі задачі» і рефакторинг в «одне виділення на старті».
// Нічого підключати не треба. Serial Plotter: free_kb, largest_kb, alloc_fail.
//
// Приклад: задача обробляє кадри змінної довжини (256 Б - 12 КБ), рахує контрольну суму
// і веде журнал останніх подій (записи 16-128 Б). Кадри короткоживучі, записи журналу -
// довгоживучі. Саме така суміш (різний розмір + різний час життя) і ріже heap на шматки.
// Щоб ефект було видно за хвилину, а не за тиждень, «баласт» лишає програмі лише 48 КБ heap
// (наче решту вже забрали Wi-Fi, BLE і драйвери).
//
// ALLOC_ONCE - змінити, прошити знову, порівняти графіки:
//   0 - ANTIPATTERN: malloc(len) на кожен кадр і на кожен запис журналу.
//       largest_kb скаче і просідає, з часом зʼявляються alloc_fail; час malloc різний щоразу.
//   1 - ПРАВИЛЬНО: буфер кадру під найгірший випадок виділено ОДИН раз при старті
//       (heap_caps_malloc з явним прапором + перевірка NULL), журнал - статичний масив
//       записів фіксованого розміру. У циклі задачі - жодного malloc, лінії на графіку рівні.

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "practice.h"

#define ALLOC_ONCE 1

#define FRAME_MIN_B        UINT32_C(256)
#define FRAME_MAX_B        (UINT32_C(12) * BYTES_PER_KB)
#define FRAME_PERIOD_MS    UINT32_C(10)
#define LOG_EVERY_FRAMES   UINT32_C(3)   // кожен третій кадр лишає запис у журналі
#define LOG_SLOTS          UINT32_C(32)  // журнал памʼятає останні 32 записи
#define LOG_MIN_B          UINT32_C(16)
#define LOG_MAX_B          UINT32_C(128)
#define ARENA_LEAVE_B      (UINT32_C(48) * BYTES_PER_KB) // стільки heap лишає баласт
#define REPORT_PERIOD_MS   UINT32_C(1000)
#define FRAMES_PER_REPORT  (REPORT_PERIOD_MS / FRAME_PERIOD_MS)
#define FRAME_STACK_BYTES  UINT32_C(4096)
#define PRIO_FRAME         ((UBaseType_t)3)

static_assert(FRAME_MAX_B < ARENA_LEAVE_B, "кадр має влазити в залишок heap");

static const char *TAG = "once";

static heap_ballast_t s_ballast;

typedef struct {
  uint32_t frames;
  uint32_t alloc_fail;          // накопичувально з початку роботи
  uint32_t alloc_cycles_min;    // за останній звіт
  uint32_t alloc_cycles_max;
} frame_stats_t;

static uint32_t random_between(uint32_t lo, uint32_t hi) {
  return lo + (esp_random() % (hi - lo + UINT32_C(1)));
}

// «Обробка кадру»: заповнити і порахувати суму, щоб буфер справді використовувався.
static uint32_t process_frame(uint8_t *buf, uint32_t len, uint32_t seq) {
  memset(buf, (int)(seq & UINT32_C(0xFF)), len);
  uint32_t sum = 0;
  for (uint32_t i = 0; i < len; i++) {
    sum += buf[i];
  }
  return sum;
}

#if ALLOC_ONCE
// ---------------- ПРАВИЛЬНО: виділили один раз, далі перевикористовуємо ----------------
typedef struct {
  uint8_t len;
  uint8_t data[LOG_MAX_B]; // фіксований розмір під найдовший запис
} log_entry_t;

static uint8_t *s_frame_buf;               // з heap, але лише один раз - при старті
static log_entry_t s_log[LOG_SLOTS];       // журнал у .bss: 32 x 129 Б, heap не чіпає
static uint32_t s_log_head;

static void log_remember(uint32_t sum) {
  log_entry_t *e = &s_log[s_log_head];
  e->len = (uint8_t)random_between(LOG_MIN_B, LOG_MAX_B);
  memset(e->data, (int)(sum & UINT32_C(0xFF)), e->len);
  s_log_head = (s_log_head + UINT32_C(1)) % LOG_SLOTS;
}

static bool frames_init(void) {
  // Init-time allocation: поки heap ще суцільний і задачі не конкурують.
  // Явний прапор: буфер у внутрішній SRAM (з PSRAM обробка була б повільнішою).
  s_frame_buf = heap_caps_malloc(FRAME_MAX_B, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return s_frame_buf != NULL;
}

static uint8_t *frame_buf_get(uint32_t len, frame_stats_t *st) {
  (void)len; // буфер уже під найгірший випадок
  (void)st;
  return s_frame_buf;
}

static void frame_buf_put(uint8_t *buf) {
  (void)buf; // нічого не звільняємо: буфер живе весь час роботи
}

#else
// ---------------- ANTIPATTERN: malloc/free на кожній ітерації ----------------
// Компілюється без попереджень і навіть працює. Погане - в архітектурі, не в синтаксисі.
static void *s_log_ptr[LOG_SLOTS];
static uint32_t s_log_head;

static void log_remember(uint32_t sum) {
  free(s_log_ptr[s_log_head]); // найстаріший запис геть (free(NULL) - нічого не робить)
  const uint32_t len = random_between(LOG_MIN_B, LOG_MAX_B);
  uint8_t *rec = malloc(len);  // ANTIPATTERN: довгоживучий дрібний блок посеред кадрів
  if (rec != NULL) {
    memset(rec, (int)(sum & UINT32_C(0xFF)), len);
  }
  s_log_ptr[s_log_head] = rec;
  s_log_head = (s_log_head + UINT32_C(1)) % LOG_SLOTS;
}

static bool frames_init(void) {
  return true; // «навіщо щось готувати, виділимо, коли знадобиться»
}

static uint8_t *frame_buf_get(uint32_t len, frame_stats_t *st) {
  const uint32_t t0 = esp_cpu_get_cycle_count();
  uint8_t *buf = malloc(len); // ANTIPATTERN: шукаємо місце в heap на кожен кадр
  const uint32_t cycles = esp_cpu_get_cycle_count() - t0;
  if (cycles < st->alloc_cycles_min) {
    st->alloc_cycles_min = cycles;
  }
  if (cycles > st->alloc_cycles_max) {
    st->alloc_cycles_max = cycles;
  }
  if (buf == NULL) {
    st->alloc_fail++;
  }
  return buf;
}

static void frame_buf_put(uint8_t *buf) {
  free(buf);
}
#endif

static void frame_task(void *arg) {
  (void)arg;
  frame_stats_t st = {.alloc_cycles_min = UINT32_MAX};
  for (uint32_t seq = 1;; seq++) {
    const uint32_t len = random_between(FRAME_MIN_B, FRAME_MAX_B);
    uint8_t *buf = frame_buf_get(len, &st);
    if (buf != NULL) { // NULL - кадр втрачено (soft fail), рахуємо і йдемо далі
      const uint32_t sum = process_frame(buf, len, seq);
      if ((seq % LOG_EVERY_FRAMES) == 0) {
        log_remember(sum);
      }
      frame_buf_put(buf);
      st.frames++;
    }

    if ((seq % FRAMES_PER_REPORT) == 0) {
      printf("free_kb:%" PRIu32 " largest_kb:%" PRIu32 " alloc_fail:%" PRIu32 "\n",
             esp_get_free_heap_size() / BYTES_PER_KB,
             (uint32_t)heap_caps_get_largest_free_block(HEAP_CAPS_MALLOC) / BYTES_PER_KB, st.alloc_fail);
      if (!ALLOC_ONCE) {
        ESP_LOGI(TAG, "malloc за останню секунду: від %" PRIu32 " до %" PRIu32 " тактів CPU", st.alloc_cycles_min,
                 st.alloc_cycles_max);
      }
      st.alloc_cycles_min = UINT32_MAX;
      st.alloc_cycles_max = 0;
    }
    vTaskDelay(pdMS_TO_TICKS(FRAME_PERIOD_MS));
  }
}

void app_main(void) {
  ESP_LOGI(TAG, "режим: %s", ALLOC_ONCE ? "одне виділення на старті" : "ANTIPATTERN malloc/free у циклі");
  log_heap(TAG, "старт");

  heap_ballast_take(&s_ballast, ARENA_LEAVE_B);
  log_heap(TAG, "баласт: програмі лишилось");

  if (!frames_init()) {
    // Буфер кадрів - критичний ресурс: без нього пристрій марний. Падаємо одразу і голосно,
    // при старті, а не через тиждень роботи.
    ESP_LOGE(TAG, "немає памʼяті під буфер кадрів");
    abort();
  }
  log_heap(TAG, "після ініціалізації");

  check_task_created(xTaskCreate(frame_task, "frames", FRAME_STACK_BYTES, NULL, PRIO_FRAME, NULL), "frames");
}
