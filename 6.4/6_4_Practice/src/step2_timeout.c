// Крок 2: той самий турнікет, але як у житті - третій стан, таймаути і світлодіоди.
//
// Що нового порівняно з кроком 1:
//   - стан ALARM: штовхнули закритий турнікет - тривога на 3 с;
//   - подія TIMEOUT: відкритий турнікет сам закривається, якщо 5 с ніхто не пройшов;
//   - дії входу в стан (entry action): колір RGB-світлодіода задається не в кожному case, а в одному місці;
//   - внутрішні переходи: подія, яка не міняє стан, не продовжує час перебування в ньому;
//   - відлік заново як дія: у тривозі кожен новий поштовх продовжує її ще на 3 с.
//
//                 CARD / списати оплату
//   +--------+ -------------------------> +----------+
//   | LOCKED |                            | UNLOCKED |  CARD / «вже оплачено»
//   +--------+ <------------------------- +----------+  (відлік 5 с не скидається)
//     |    ^    PUSH / +1 пасажир
//     |    |    TIMEOUT 5 с / «ніхто не пройшов»
//     |    |
//     |    | TIMEOUT 3 с
//     |    |
//     |  +-------+  CARD / ігноруємо (відлік 3 с не скидається)
//     +->| ALARM |
//  PUSH  +-------+  PUSH / тривога ще 3 с (відлік заново)
//
// Вбудований RGB-світлодіод (колір залежить лише від стану - автомат Мура):
//   LOCKED - ледь синій, UNLOCKED - зелений, ALARM - червоний.
//
// Best practices цього кроку (на додачу до кроку 1):
//   1. Час - це теж подія. Автомат не викликає vTaskDelay і не перевіряє «скільки минуло»
//      у кожному case: таймаут приходить як звичайна подія EV_TIMEOUT і обробляється тим самим switch.
//   2. Таймаут рахується від ВХОДУ в стан, а не від останньої події. Інакше пасажир,
//      який щосекунди прикладає картку, тримав би турнікет відкритим вічно.
//      Запамʼятовуємо тік входу (xTaskGetTickCount) і щоразу рахуємо, скільки лишилось, -
//      та сама беззнакова арифметика, що з millis() в Arduino.
//      Якщо ж подія МАЄ продовжити таймаут («поки штовхають - тривога триває»), це окрема
//      явна дія переходу deadline_restart(). Вирішує автор автомата, а не випадковість.
//   3. Дії входу - в одній функції enter_state(). Хоч скільки переходів у стан UNLOCKED,
//      зелений колір вмикається в одному місці. Додали новий перехід - колір не забудеться.
//   4. Жодного блокування всередині обробки події: обробка - мікросекунди, решту часу
//      задача спить у xQueueReceive (стан Blocked).
//
// Залізо - лише плата: вбудований RGB-світлодіод (WS2812, драйвер у common_rgb_led.c).

#include <assert.h>
#include <inttypes.h>
#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "practice.h"
#include "rgb_led.h"

static const char *TAG = "turnstile";

// --- Кольори станів (неяскраві: вбудований світлодіод сліпить на повній потужності) ---
#define COLOR_LOCKED   ((rgb_t){0, 0, 8})  // ледь синій: живий, закритий
#define COLOR_UNLOCKED ((rgb_t){0, 30, 0}) // зелений: проходьте
#define COLOR_ALARM    ((rgb_t){40, 0, 0}) // червоний: тривога

// --- Таймаути станів ---
#define UNLOCKED_TIMEOUT_MS UINT32_C(5000) // скільки чекаємо пасажира після оплати
#define ALARM_TIMEOUT_MS    UINT32_C(3000) // скільки триває тривога

static_assert(UNLOCKED_TIMEOUT_MS >= 1000, "людині треба хоча б секунда, щоб пройти");
static_assert(ALARM_TIMEOUT_MS > 0, "тривога без тривалості - не тривога");

// --- 1. Словник автомата ---

typedef enum {
  TS_LOCKED,   // закрито, чекаємо оплату
  TS_UNLOCKED, // оплачено, чекаємо пасажира
  TS_ALARM,    // спроба пройти без оплати
} ts_state_t;

typedef enum {
  EV_CARD,    // приклали картку
  EV_PUSH,    // штовхнули планку турнікета
  EV_TIMEOUT, // минув таймаут поточного стану (генерує сам автомат, не консоль)
} ts_event_t;

static const char *state_name(ts_state_t state) {
  switch (state) {
    case TS_LOCKED:   return "LOCKED";
    case TS_UNLOCKED: return "UNLOCKED";
    case TS_ALARM:    return "ALARM";
  }
  return "?";
}

static const char *event_name(ts_event_t event) {
  switch (event) {
    case EV_CARD:    return "CARD";
    case EV_PUSH:    return "PUSH";
    case EV_TIMEOUT: return "TIMEOUT";
  }
  return "?";
}

// Скільки автомат може пробути в стані до події TIMEOUT. portMAX_DELAY - без обмеження.
static TickType_t state_timeout(ts_state_t state) {
  switch (state) {
    case TS_LOCKED:   return portMAX_DELAY;
    case TS_UNLOCKED: return pdMS_TO_TICKS(UNLOCKED_TIMEOUT_MS);
    case TS_ALARM:    return pdMS_TO_TICKS(ALARM_TIMEOUT_MS);
  }
  return portMAX_DELAY;
}

// --- 2. Дані, якими керує автомат ---
static uint32_t s_fares;
static uint32_t s_passengers;

// Дедлайн стану: з якого тіку почали відлік і скільки тіків можна пробути без подій.
typedef struct {
  TickType_t start; // xTaskGetTickCount() у момент входу в стан (або перезапуску відліку)
  TickType_t limit; // таймаут стану; portMAX_DELAY - без обмеження
} state_deadline_t;

// Починає відлік таймауту стану з нуля.
static void deadline_restart(state_deadline_t *deadline, ts_state_t state) {
  deadline->start = xTaskGetTickCount();
  deadline->limit = state_timeout(state);
}

// Скільки тіків лишилось до TIMEOUT. 0 - час уже вийшов.
// now - start - беззнакове віднімання: правильне і після переповнення лічильника тіків,
// як now - last з millis() в Arduino.
static TickType_t deadline_ticks_left(const state_deadline_t *deadline) {
  if (deadline->limit == portMAX_DELAY) {
    return portMAX_DELAY;
  }
  TickType_t elapsed = xTaskGetTickCount() - deadline->start;
  return (elapsed >= deadline->limit) ? 0 : deadline->limit - elapsed;
}

// Колір світлодіода залежить лише від стану.
static rgb_t state_color(ts_state_t state) {
  switch (state) {
    case TS_LOCKED:   return COLOR_LOCKED;
    case TS_UNLOCKED: return COLOR_UNLOCKED;
    case TS_ALARM:    return COLOR_ALARM;
  }
  return RGB_OFF;
}

// --- 3. Дія входу в стан: вихід (колір) і початок відліку таймауту ---
static void enter_state(ts_state_t state, state_deadline_t *deadline) {
  rgb_led_set(state_color(state));
  deadline_restart(deadline, state);
}

// --- 4. Правила: (стан, подія) -> дія + наступний стан ---
// deadline передаємо лише для дії «почати відлік заново»; змінювати стан тут не можна.
static ts_state_t handle_event(ts_state_t state, ts_event_t event, state_deadline_t *deadline) {
  switch (state) {
    case TS_LOCKED:
      switch (event) {
        case EV_CARD:
          s_fares++;
          ESP_LOGI(TAG, "Оплату списано (усього %" PRIu32 "). Проходьте.", s_fares);
          return TS_UNLOCKED;
        case EV_PUSH:
          ESP_LOGW(TAG, "Спроба пройти без оплати!");
          return TS_ALARM;
        case EV_TIMEOUT:
          return TS_LOCKED; // у LOCKED таймауту немає; обробляємо явно, щоб компілятор не сварився
      }
      break;

    case TS_UNLOCKED:
      switch (event) {
        case EV_CARD:
          ESP_LOGI(TAG, "Уже оплачено, вдруге не списуємо.");
          return TS_UNLOCKED; // внутрішній перехід: відлік 5 с НЕ починається заново
        case EV_PUSH:
          s_passengers++;
          ESP_LOGI(TAG, "Пасажир пройшов (усього %" PRIu32 ").", s_passengers);
          return TS_LOCKED;
        case EV_TIMEOUT:
          ESP_LOGI(TAG, "%" PRIu32 " мс ніхто не пройшов, закриваємо.", UNLOCKED_TIMEOUT_MS);
          return TS_LOCKED;
      }
      break;

    case TS_ALARM:
      switch (event) {
        case EV_CARD:
          ESP_LOGW(TAG, "Тривога, зачекайте.");
          return TS_ALARM; // внутрішній перехід: відлік тривоги йде далі
        case EV_PUSH:
          ESP_LOGW(TAG, "Знову штовхають! Тривога ще %" PRIu32 " мс.", ALARM_TIMEOUT_MS);
          deadline_restart(deadline, TS_ALARM); // активність продовжує таймаут
          return TS_ALARM;
        case EV_TIMEOUT:
          ESP_LOGI(TAG, "Тривогу знято.");
          return TS_LOCKED;
      }
      break;
  }
  return state;
}

// --- 5. Задача-автомат (Consumer) ---

static QueueHandle_t s_events;

// Наступна подія: з черги або TIMEOUT, якщо час стану вийшов раніше.
// Чекаємо лише залишок від входу в стан, тому подія, яка не змінила стан, не продовжує таймаут.
static ts_event_t wait_event(const state_deadline_t *deadline) {
  ts_event_t event;
  TickType_t left = deadline_ticks_left(deadline);
  if (left == 0) {
    return EV_TIMEOUT; // час уже вийшов (наприклад, поки обробляли попередню подію)
  }
  if (xQueueReceive(s_events, &event, left) != pdPASS) {
    return EV_TIMEOUT; // за відведений час подій не було
  }
  return event;
}

static void fsm_task(void *arg) {
  (void)arg;
  ts_state_t state = TS_LOCKED;
  state_deadline_t deadline;
  enter_state(state, &deadline); // дія входу - і для початкового стану теж
  ESP_LOGI(TAG, "Старт у стані %s", state_name(state));

  for (;;) {
    ts_event_t event = wait_event(&deadline);
    ts_state_t next = handle_event(state, event, &deadline);
    if (next != state) {
      ESP_LOGI(TAG, "%s --%s--> %s", state_name(state), event_name(event), state_name(next));
      state = next;
      enter_state(state, &deadline); // лише при справжньому переході
    }
  }
}

// --- 6. Джерело подій (Producer): клавіші з консолі ---
// EV_TIMEOUT консоль не надсилає: час - справа самого автомата.

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
      ESP_LOGW(TAG, "Черга подій повна, подію %s втрачено", event_name(event));
    }
  }
}

// --- 7. Ініціалізація ---

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
