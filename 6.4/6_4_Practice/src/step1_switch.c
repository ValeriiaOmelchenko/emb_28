// Крок 1: найпростіший кінцевий автомат - турнікет у метро. Два стани, дві події, switch.
//
// Кінцевий автомат (FSM) - це:
//   - скінченний набір СТАНІВ (у кожен момент автомат рівно в одному з них);
//   - скінченний набір ПОДІЙ (що може статися ззовні);
//   - ПРАВИЛА: «у стані S прийшла подія E -> виконати дію і перейти в стан S'».
// Більше нічого. Жодних прапорців is_paid / was_pushed / is_open, які треба тримати узгодженими.
// Аналогія: турнікет не «памʼятає історію» - він просто зараз або закритий, або відкритий.
//
//              CARD / списати оплату
//   +--------+ ---------------------> +----------+
//   | LOCKED |                        | UNLOCKED |
//   +--------+ <--------------------- +----------+
//    |    ^       PUSH / +1 пасажир      |    ^
//    +----+                              +----+
//    PUSH / «прохід заборонено»          CARD / «вже оплачено»
//
// Клавіші в Serial Monitor: c - прикласти картку (CARD), p - штовхнути турнікет (PUSH).
//
// Best practices цього кроку:
//   1. Стани і події - enum, а не магічні числа чи рядки.
//   2. Стан - локальна змінна задачі-автомата. Один власник, ніяких глобальних прапорців і гонок.
//   3. Автомат не знає, звідки події: консоль, кнопка чи MQTT - для нього це просто черга.
//   4. switch без default: якщо додати новий стан чи подію і забути обробити,
//      компілятор попередить (-Wswitch, входить у -Wall).
//   5. Функція обробки повертає наступний стан, а присвоює його ОДНЕ місце в коді.
//      Там же - єдиний лог переходів: «LOCKED --CARD--> UNLOCKED».

#include <inttypes.h>
#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "practice.h"

static const char *TAG = "turnstile";

// --- 1. Словник автомата: стани і події ---

typedef enum {
  TS_LOCKED,   // закрито, чекаємо оплату
  TS_UNLOCKED, // оплачено, чекаємо пасажира
} ts_state_t;

typedef enum {
  EV_CARD, // приклали картку
  EV_PUSH, // штовхнули планку турнікета
} ts_event_t;

// Імена для логу. switch замість масиву рядків: забули новий стан - компілятор нагадає.
static const char *state_name(ts_state_t state) {
  switch (state) {
    case TS_LOCKED:   return "LOCKED";
    case TS_UNLOCKED: return "UNLOCKED";
  }
  return "?"; // сюди не дійдемо, якщо у змінній коректне значення enum
}

static const char *event_name(ts_event_t event) {
  switch (event) {
    case EV_CARD: return "CARD";
    case EV_PUSH: return "PUSH";
  }
  return "?";
}

// --- 2. Дані, якими керує автомат (не стан!) ---
// Лічильники - це «що порахували», а не «де ми зараз». Стан автомата від них не залежить.
static uint32_t s_fares;      // скільки разів списали оплату
static uint32_t s_passengers; // скільки людей пройшло

// --- 3. Правила: (стан, подія) -> дія + наступний стан ---
// Зовнішній switch - по стану, внутрішній - по події. Кожна клітинка таблиці переходів
// з README - рівно один case. Читається як «у стані X на подію Y робимо Z».
static ts_state_t handle_event(ts_state_t state, ts_event_t event) {
  switch (state) {
    case TS_LOCKED:
      switch (event) {
        case EV_CARD:
          s_fares++;
          ESP_LOGI(TAG, "Оплату списано (усього %" PRIu32 "). Проходьте.", s_fares);
          return TS_UNLOCKED;
        case EV_PUSH:
          ESP_LOGW(TAG, "Прохід заборонено: спочатку прикладіть картку.");
          return TS_LOCKED;
      }
      break;

    case TS_UNLOCKED:
      switch (event) {
        case EV_CARD:
          // Друга оплата за той самий прохід - помилка, яку легко зробити з прапорцями.
          ESP_LOGI(TAG, "Уже оплачено, вдруге не списуємо.");
          return TS_UNLOCKED;
        case EV_PUSH:
          s_passengers++;
          ESP_LOGI(TAG, "Пасажир пройшов (усього %" PRIu32 ").", s_passengers);
          return TS_LOCKED;
      }
      break;

    default:
      ESP_LOGI(TAG, "Невизначений стан");
      break;
  }
  return state; // захист від некоректного значення: лишаємось, де були
}

// --- 4. Задача-автомат (Consumer) ---

static QueueHandle_t s_events;

static void fsm_task(void *arg) {
  (void)arg;
  ts_state_t state = TS_LOCKED; // початковий стан задаємо явно
  ESP_LOGI(TAG, "Старт у стані %s", state_name(state));

  for (;;) {
    ts_event_t event;
    if (xQueueReceive(s_events, &event, portMAX_DELAY) != pdPASS) {
      continue;
    }

    ts_state_t next = handle_event(state, event);
    if (next != state) {
      ESP_LOGI(TAG, "%s --%s--> %s", state_name(state), event_name(event), state_name(next));
    }
    state = next; // ЄДИНЕ місце, де змінюється стан
  }
}

// --- 5. Джерело подій (Producer): клавіші з консолі ---
// Перекладає «сирі» символи в події автомата. Невідомі клавіші сюди й відсіюються,
// автомат їх навіть не бачить.

static void console_task(void *arg) {
  (void)arg;
  for (;;) {
    char key = console_read_key();
    ts_event_t event;
    switch (key) {
      case 'c': case 'C': 
        event = EV_CARD; 
        break;
      case 'p': case 'P': 
        event = EV_PUSH; 
        break;
      default:
        ESP_LOGI(TAG, "Клавіші: c - картка, p - пройти");
        continue;
    }
    if (xQueueSend(s_events, &event, 0) != pdPASS) {
      ESP_LOGW(TAG, "Черга подій повна, подію %s втрачено", event_name(event));
    }
  }
}

// --- 6. Ініціалізація ---

void app_main(void) {
  // Черга статична: памʼять під неї виділена під час компіляції, heap не потрібен.
  static StaticQueue_t queue_struct;
  static uint8_t queue_storage[EVENT_QUEUE_LENGTH * sizeof(ts_event_t)];
  s_events = xQueueCreateStatic(EVENT_QUEUE_LENGTH, sizeof(ts_event_t), queue_storage, &queue_struct);

  console_init();

  // Спершу споживач, потім джерело: перша подія не прийде в нікуди.
  check_task_created(xTaskCreate(fsm_task, "fsm", LOG_TASK_STACK_BYTES, NULL, PRIO_FSM, NULL), "fsm");
  check_task_created(xTaskCreate(console_task, "console", LOG_TASK_STACK_BYTES, NULL, PRIO_CONSOLE, NULL), "console");
  ESP_LOGI(TAG, "Клавіші: c - картка, p - пройти");
}
