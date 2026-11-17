// Крок 3: поведінка - точнісінько як у кроці 2, але автомат описано ТАБЛИЦЕЮ, а не switch.
//
// Таблиця переходів з README стає масивом у коді: рядок - стан, стовпець - подія,
// у клітинці - дія і наступний стан. Код «двигуна» автомата (dispatch) - 10 рядків і більше
// не змінюється: новий стан чи подія - це нові рядки таблиці, а не новий case у трьох місцях.
// Аналогія: розклад електричок. Машиніст не тримає його в голові, а дивиться в таблицю.
//
// Best practices цього кроку:
//   1. Таблиці - static const: лежать у флеші, а не в RAM, і їх неможливо випадково зіпсувати.
//   2. Designated initializers ([TS_LOCKED] = ..., [EV_CARD] = ...): порядок рядків не важливий,
//      а клітинку видно за назвою, не за номером.
//   3. Порожня клітинка = нуль = .handled == false = «подію в цьому стані ігноруємо».
//      Безпечне значення за замовчуванням: забута клітинка нічого не зламає, лише залогується.
//   4. static_assert перевіряє, що таблиця імен має рівно STATE_COUNT рядків.
//   5. Дії - маленькі функції без побічних переходів: дія ніколи сама не міняє стан.
//
// Коли що брати:
//   - switch (кроки 1-2): мало станів, у переходах різна логіка з умовами. Легко читати і дебажити.
//   - таблиця (крок 3): багато станів і подій, переходи однотипні. Видно всю поведінку одним поглядом,
//     таблицю легко звірити з діаграмою (і навіть згенерувати з неї).
//
// Залізо - лише плата: вбудований RGB-світлодіод, як у кроці 2.

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "practice.h"
#include "rgb_led.h"

static const char *TAG = "turnstile";

// --- Таймаути станів ---
#define UNLOCKED_TIMEOUT_MS UINT32_C(5000)
#define ALARM_TIMEOUT_MS    UINT32_C(3000)

static_assert(UNLOCKED_TIMEOUT_MS >= 1000, "людині треба хоча б секунда, щоб пройти");
static_assert(ALARM_TIMEOUT_MS > 0, "тривога без тривалості - не тривога");

// --- 1. Словник автомата ---
// Тут *_COUNT - член enum: switch по цих enum у кроці немає, а розмір таблиць рахується сам.

typedef enum {
  TS_LOCKED,
  TS_UNLOCKED,
  TS_ALARM,
  TS_STATE_COUNT
} ts_state_t;

typedef enum {
  EV_CARD,
  EV_PUSH,
  EV_TIMEOUT,
  EV_COUNT
} ts_event_t;

static const char *const EVENT_NAMES[] = {
    [EV_CARD] = "CARD",
    [EV_PUSH] = "PUSH",
    [EV_TIMEOUT] = "TIMEOUT",
};
static_assert(sizeof(EVENT_NAMES) / sizeof(EVENT_NAMES[0]) == EV_COUNT, "імʼя для кожної події");

// --- 2. Опис станів: усе, що належить стану, а не переходу ---
// Колір і таймаут - дані, а не код. Дія входу enter_state() просто їх застосовує.

typedef struct {
  const char *name;
  TickType_t timeout; // portMAX_DELAY - без таймауту
  rgb_t color;        // колір вбудованого світлодіода в цьому стані
} state_info_t;

static const state_info_t STATES[] = {
    [TS_LOCKED]   = {.name = "LOCKED",   .timeout = portMAX_DELAY,                      .color = {0, 0, 8}},
    [TS_UNLOCKED] = {.name = "UNLOCKED", .timeout = pdMS_TO_TICKS(UNLOCKED_TIMEOUT_MS), .color = {0, 30, 0}},
    [TS_ALARM]    = {.name = "ALARM",    .timeout = pdMS_TO_TICKS(ALARM_TIMEOUT_MS),    .color = {40, 0, 0}},
};
static_assert(sizeof(STATES) / sizeof(STATES[0]) == TS_STATE_COUNT, "опис для кожного стану");

// --- 3. Дії на переходах ---
// Кожна дія - одна маленька функція. Стан вона не змінює: це робить лише dispatch().

static uint32_t s_fares;
static uint32_t s_passengers;

static void act_charge(void) {
  s_fares++;
  ESP_LOGI(TAG, "Оплату списано (усього %" PRIu32 "). Проходьте.", s_fares);
}

static void act_already_paid(void) {
  ESP_LOGI(TAG, "Уже оплачено, вдруге не списуємо.");
}

static void act_pass(void) {
  s_passengers++;
  ESP_LOGI(TAG, "Пасажир пройшов (усього %" PRIu32 ").", s_passengers);
}

static void act_nobody_passed(void) {
  ESP_LOGI(TAG, "%" PRIu32 " мс ніхто не пройшов, закриваємо.", UNLOCKED_TIMEOUT_MS);
}

static void act_intrusion(void) {
  ESP_LOGW(TAG, "Спроба пройти без оплати!");
}

static void act_wait_alarm(void) {
  ESP_LOGW(TAG, "Тривога, зачекайте.");
}

static void act_push_in_alarm(void) {
  ESP_LOGW(TAG, "Знову штовхають! Тривога ще %" PRIu32 " мс.", ALARM_TIMEOUT_MS);
}

static void act_alarm_off(void) {
  ESP_LOGI(TAG, "Тривогу знято.");
}

// --- 4. Таблиця переходів: та сама, що в README, рядок у рядок ---

typedef struct {
  bool handled;         // false (порожня клітинка) - подію ігноруємо
  ts_state_t next;      // куди переходимо; той самий стан - внутрішній перехід
  void (*action)(void); // що робимо на переході; NULL - нічого
  bool restart_timeout; // лише для next == поточний стан: подія починає відлік таймауту заново
} transition_t;

static const transition_t TRANSITIONS[TS_STATE_COUNT][EV_COUNT] = {
    [TS_LOCKED] = {
        [EV_CARD]    = {true, TS_UNLOCKED, act_charge},
        [EV_PUSH]    = {true, TS_ALARM,    act_intrusion},
        // EV_TIMEOUT: у LOCKED таймауту немає - клітинка порожня
    },
    [TS_UNLOCKED] = {
        [EV_CARD]    = {true, TS_UNLOCKED, act_already_paid},
        [EV_PUSH]    = {true, TS_LOCKED,   act_pass},
        [EV_TIMEOUT] = {true, TS_LOCKED,   act_nobody_passed},
    },
    [TS_ALARM] = {
        [EV_CARD]    = {true, TS_ALARM,    act_wait_alarm},
        [EV_PUSH]    = {true, TS_ALARM,    act_push_in_alarm, .restart_timeout = true},
        [EV_TIMEOUT] = {true, TS_LOCKED,   act_alarm_off},
    },
};

// --- 5. Двигун автомата: однаковий для будь-якої таблиці ---

typedef struct {
  ts_state_t state;
  TickType_t start;   // тік входу в стан (або початку відліку заново)
} fsm_t;

static void deadline_restart(fsm_t *fsm) {
  fsm->start = xTaskGetTickCount();
}

// Скільки тіків лишилось до TIMEOUT; 0 - час вийшов. Беззнакове віднімання, як з millis().
static TickType_t deadline_ticks_left(const fsm_t *fsm) {
  TickType_t limit = STATES[fsm->state].timeout;
  if (limit == portMAX_DELAY) {
    return portMAX_DELAY;
  }
  TickType_t elapsed = xTaskGetTickCount() - fsm->start;
  return (elapsed >= limit) ? 0 : limit - elapsed;
}

static void enter_state(fsm_t *fsm, ts_state_t state) {
  const state_info_t *info = &STATES[state];
  fsm->state = state;
  rgb_led_set(info->color);
  deadline_restart(fsm);
}

static void dispatch(fsm_t *fsm, ts_event_t event) {
  if (fsm->state >= TS_STATE_COUNT || event >= EV_COUNT) {
    ESP_LOGE(TAG, "Некоректний стан %d або подія %d", (int)fsm->state, (int)event);
    return; // захист від виходу за межі таблиці
  }

  const transition_t *t = &TRANSITIONS[fsm->state][event];
  if (!t->handled) {
    ESP_LOGI(TAG, "%s: подію %s проігноровано", STATES[fsm->state].name, EVENT_NAMES[event]);
    return;
  }

  if (t->action != NULL) {
    t->action();
  }
  if (t->next != fsm->state) {
    ESP_LOGI(TAG, "%s --%s--> %s", STATES[fsm->state].name, EVENT_NAMES[event], STATES[t->next].name);
    enter_state(fsm, t->next); // справжній перехід: дія входу + відлік з нуля
  } else if (t->restart_timeout) {
    deadline_restart(fsm); // той самий стан, але подія продовжує таймаут
  }
}

// --- 6. Задача-автомат (Consumer) ---

static QueueHandle_t s_events;

static ts_event_t wait_event(const fsm_t *fsm) {
  ts_event_t event;
  TickType_t left = deadline_ticks_left(fsm);
  if (left == 0) {
    return EV_TIMEOUT;
  }
  if (xQueueReceive(s_events, &event, left) != pdPASS) {
    return EV_TIMEOUT;
  }
  return event;
}

static void fsm_task(void *arg) {
  (void)arg;
  fsm_t fsm;
  enter_state(&fsm, TS_LOCKED);
  ESP_LOGI(TAG, "Старт у стані %s", STATES[fsm.state].name);

  for (;;) {
    dispatch(&fsm, wait_event(&fsm));
  }
}

// --- 7. Джерело подій (Producer): клавіші з консолі ---

static void console_task(void *arg) {
  (void)arg;
  for (;;) {
    char key = console_read_key();
    ts_event_t event;
    switch (key) {
      case 'c': case 'C': event = EV_CARD; break;
      case 'p': case 'P': event = EV_PUSH; break;
      default:
        ESP_LOGI(TAG, "Клавіші: c - картка, p - пройти");
        continue;
    }
    if (xQueueSend(s_events, &event, 0) != pdPASS) {
      ESP_LOGW(TAG, "Черга подій повна, подію %s втрачено", EVENT_NAMES[event]);
    }
  }
}

// --- 8. Ініціалізація ---

void app_main(void) {
  static StaticQueue_t queue_struct;
  static uint8_t queue_storage[EVENT_QUEUE_LENGTH * sizeof(ts_event_t)];
  s_events = xQueueCreateStatic(EVENT_QUEUE_LENGTH, sizeof(ts_event_t), queue_storage, &queue_struct);

  rgb_led_init();
  console_init();

  check_task_created(xTaskCreate(fsm_task, "fsm", LOG_TASK_STACK_BYTES, NULL, PRIO_FSM, NULL), "fsm");
  check_task_created(xTaskCreate(console_task, "console", LOG_TASK_STACK_BYTES, NULL, PRIO_CONSOLE, NULL),
                     "console");
  ESP_LOGI(TAG, "Клавіші: c - картка, p - пройти");
}
