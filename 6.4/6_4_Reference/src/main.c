// Еталон 6.4 - кінцевий автомат: електронний сейф з однією кнопкою.
// ESP32-S3-DevKitC-1, ESP-IDF, чистий C17.
//
// Пароль: Коротке - Коротке - Довге. Помилка або 3 с бездіяльності під час вводу -> скидання.
//
//   [ button_task ] --подія--> [ черга ] --> [ safe_task ]
//   Producer: автомат кнопки          Consumer: автомат сейфа
//   (debounce, PRESS/SHORT/LONG)      (стани, таймаути, світлодіод)
//
// Автомат сейфа не знає про GPIO і брязкіт, автомат кнопки не знає про пароль. Зв'язок між
// ними - лише черга подій. Стан сейфа - локальна змінна safe_task: один власник, без гонок.
// Аналогія: кнопка - офіціант, що приносить замовлення ("коротке", "довге"), сейф - кухар,
// який дивиться лише на поточний крок рецепта.

#include <assert.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "safe";

// --- Залізо ---
// Кнопка між GPIO4 і GND, внутрішня підтяжка до 3V3 (~45 кОм). Довгий провід або шумне
// оточення - ставимо ще зовнішні 10 кОм до 3V3: внутрішня підтяжка слабка (Trust No Pin).
// GPIO0 (BOOT) не беремо: це strapping-пін, затиснутий при скиданні - плата йде в режим прошивки.
#define BUTTON_PIN       GPIO_NUM_4
#define BUTTON_PRESSED   0 // активний низький рівень: натиснуто = 0
// Світлодіод "відчинено": GPIO5 -> 220 Ом -> LED -> GND. (3,3 - 2,0) В / 220 Ом ≈ 6 мА.
#define LED_OPEN_PIN     GPIO_NUM_5

// --- Час ---
#define POLL_PERIOD_MS    UINT32_C(10)   // опитування кнопки, строго періодичне
#define DEBOUNCE_MS       UINT32_C(30)   // рівень має бути стабільним стільки, щоб його прийняти
#define LONG_PRESS_MS     UINT32_C(1000) // утримання від цього часу = "довге"
#define INPUT_TIMEOUT_MS  UINT32_C(3000) // "затупив": скільки чекаємо наступну подію під час вводу
#define OPEN_TIME_MS      UINT32_C(5000) // сейф відчинено стільки, потім замикається сам

// Брязкіт контактів зазвичай 1-10 мс, вікно 30 мс = 3 опитування поспіль.
static_assert(DEBOUNCE_MS >= 2 * POLL_PERIOD_MS, "debounce must span at least 2 polls");
// LONG надсилається в момент, коли утримання досягло LONG_PRESS_MS. Якщо таймаут коротший,
// сейф скинеться, поки користувач чесно тримає кнопку.
static_assert(LONG_PRESS_MS < INPUT_TIMEOUT_MS, "timeout must not fire while button is held");
static_assert(POLL_PERIOD_MS * configTICK_RATE_HZ >= 1000, "poll period shorter than one tick");

// --- Задачі й черга ---
#define EVENT_QUEUE_LEN    ((UBaseType_t)8) // людина дає <= 3 події/с, 8 - запас із головою
#define BUTTON_TASK_PRIO   ((UBaseType_t)4)
#define SAFE_TASK_PRIO     ((UBaseType_t)5) // подію обробляємо одразу, як прийшла
#define TASK_STACK_BYTES   UINT32_C(4096)

// Події автомата кнопки. У черзі лежить uint8_t: enum у C займає 4 байти, а нам вистачає одного.
typedef enum {
    EV_PRESS = 0, // кнопку натиснули (після debounce) - "користувач щось робить"
    EV_SHORT,     // відпустили раніше LONG_PRESS_MS
    EV_LONG,      // утримання досягло LONG_PRESS_MS (не чекаємо відпускання)
    EV_TIMEOUT,   // внутрішня подія автомата сейфа, у черзі не буває
    EV_COUNT
} safe_event_t;

typedef enum {
    SAFE_LOCKED = 0, // чекаємо перше натискання, таймауту немає
    SAFE_GOT_S,      // отримали "коротке"
    SAFE_GOT_SS,     // отримали "коротке, коротке"
    SAFE_OPEN,       // відчинено, світлодіод горить
    SAFE_COUNT
} safe_state_t;

static const char *const EVENT_NAME[EV_COUNT] = {
    [EV_PRESS] = "PRESS", [EV_SHORT] = "SHORT", [EV_LONG] = "LONG", [EV_TIMEOUT] = "TIMEOUT",
};
static const char *const STATE_NAME[SAFE_COUNT] = {
    [SAFE_LOCKED] = "LOCKED", [SAFE_GOT_S] = "GOT_S", [SAFE_GOT_SS] = "GOT_SS", [SAFE_OPEN] = "OPEN",
};

static StaticQueue_t s_queue_struct;
static uint8_t s_queue_storage[EVENT_QUEUE_LEN * sizeof(uint8_t)];
static QueueHandle_t s_events;

// Продюсер не зміг покласти подію в чергу. Послідовність зламана, автомат сейфа має скинутись,
// а не прийняти "S L" замість "S S L".
static atomic_bool s_event_lost = false;

// ============================ Producer: автомат кнопки ============================

static void send_event(safe_event_t ev)
{
    const uint8_t item = (uint8_t)ev;
    // Таймаут 0: опитування кнопки не має зависати через повну чергу. Втрату видно в лозі.
    if (xQueueSend(s_events, &item, 0) != pdPASS) {
        atomic_store(&s_event_lost, true);
        ESP_LOGW(TAG, "queue full, %s lost", EVENT_NAME[ev]);
    }
}

static void button_task(void *arg)
{
    (void)arg;
    const TickType_t debounce_ticks = pdMS_TO_TICKS(DEBOUNCE_MS);
    const TickType_t long_ticks = pdMS_TO_TICKS(LONG_PRESS_MS);

    int raw_last = gpio_get_level(BUTTON_PIN);
    int stable = raw_last;
    TickType_t raw_changed_at = xTaskGetTickCount();
    TickType_t pressed_at = raw_changed_at;
    bool long_sent = false;
    TickType_t wake = xTaskGetTickCount();

    for (;;) {
        // vTaskDelayUntil: період 10 мс не пливе від часу виконання і логів.
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(POLL_PERIOD_MS));
        const TickType_t now = xTaskGetTickCount();
        const int raw = gpio_get_level(BUTTON_PIN);

        // Debounce на ОБИДВА фронти: новий рівень приймаємо, лише коли він протримався
        // DEBOUNCE_MS. Кожен "стрибок" контакту перезапускає відлік.
        if (raw != raw_last) {
            raw_last = raw;
            raw_changed_at = now;
        } else if (raw != stable && (TickType_t)(now - raw_changed_at) >= debounce_ticks) {
            stable = raw;
            if (stable == BUTTON_PRESSED) {
                pressed_at = now;
                long_sent = false;
                send_event(EV_PRESS);
            } else if (!long_sent) {
                send_event(EV_SHORT);
            }
        }

        // Час - різниця тіків з ">=" (беззнакова арифметика переживає переповнення лічильника),
        // а не "hold += 10" і не "case 1000:".
        if (stable == BUTTON_PRESSED && !long_sent &&
            (TickType_t)(now - pressed_at) >= long_ticks) {
            long_sent = true;
            send_event(EV_LONG);
        }
    }
}

// ============================ Consumer: автомат сейфа ============================

// Таблиця правил. Чиста функція: без FreeRTOS, GPIO і логів - її легко перевірити очима
// або тестом на ПК. switch без default: новий стан без обробки - попередження компілятора.
static safe_state_t safe_next(safe_state_t state, safe_event_t ev)
{
    switch (state) {
    case SAFE_LOCKED:
        return (ev == EV_SHORT) ? SAFE_GOT_S : SAFE_LOCKED;
    case SAFE_GOT_S:
        if (ev == EV_PRESS) return SAFE_GOT_S; // натиснув - отже, не "затупив"
        return (ev == EV_SHORT) ? SAFE_GOT_SS : SAFE_LOCKED;
    case SAFE_GOT_SS:
        if (ev == EV_PRESS) return SAFE_GOT_SS;
        return (ev == EV_LONG) ? SAFE_OPEN : SAFE_LOCKED;
    case SAFE_OPEN:
        // Кнопка у відчиненому стані нічого не робить і таймер не продовжує: затиснута
        // кнопка не має тримати сейф відчиненим вічно.
        return (ev == EV_TIMEOUT) ? SAFE_LOCKED : SAFE_OPEN;
    case SAFE_COUNT:
        break;
    }
    return SAFE_LOCKED; // недійсний стан -> безпечний стан
}

// Скільки стан чекає без подій. 0 - чекаємо вічно (у спокої задача спить, а не прокидається
// 100 разів на секунду).
static TickType_t state_timeout(safe_state_t state)
{
    switch (state) {
    case SAFE_GOT_S:
    case SAFE_GOT_SS:
        return pdMS_TO_TICKS(INPUT_TIMEOUT_MS);
    case SAFE_OPEN:
        return pdMS_TO_TICKS(OPEN_TIME_MS);
    case SAFE_LOCKED:
    case SAFE_COUNT:
        break;
    }
    return 0;
}

// Виходи залежать лише від стану (автомат Мура) і задаються в одному місці. Світлодіод
// "відчинено" горить тільки в SAFE_OPEN - будь-яке скидання гарантовано його гасить.
static void apply_outputs(safe_state_t state)
{
    gpio_set_level(LED_OPEN_PIN, (state == SAFE_OPEN) ? 1 : 0);
}

static void log_transition(safe_state_t from, safe_event_t ev, safe_state_t to)
{
    if (to == SAFE_OPEN && from != SAFE_OPEN) {
        ESP_LOGI(TAG, "%s --%s--> %s: ACCESS GRANTED", STATE_NAME[from], EVENT_NAME[ev], STATE_NAME[to]);
    } else if (to == SAFE_LOCKED && from != SAFE_LOCKED && from != SAFE_OPEN) {
        ESP_LOGW(TAG, "%s --%s--> %s: reset", STATE_NAME[from], EVENT_NAME[ev], STATE_NAME[to]);
    } else {
        ESP_LOGI(TAG, "%s --%s--> %s", STATE_NAME[from], EVENT_NAME[ev], STATE_NAME[to]);
    }
}

static void safe_task(void *arg)
{
    (void)arg;
    safe_state_t state = SAFE_LOCKED;
    TickType_t entered_at = xTaskGetTickCount(); // момент, від якого рахується таймаут стану
    // Натискання почалось у поточному "сеансі" вводу. Якщо кнопку натиснули ще у відчиненому
    // сейфі, а відпустили вже після замикання, її SHORT/LONG не стає першим символом пароля.
    bool press_armed = false;

    apply_outputs(state);
    ESP_LOGI(TAG, "locked, password: SHORT SHORT LONG");

    for (;;) {
        // Чекаємо подію не довше, ніж стану лишилось жити. Рахуємо від входу в стан
        // (а не від останнього пробудження), тому таймаут не "розтягується".
        const TickType_t timeout = state_timeout(state);
        TickType_t wait = portMAX_DELAY;
        if (timeout != 0) {
            const TickType_t elapsed = xTaskGetTickCount() - entered_at;
            wait = (elapsed >= timeout) ? 0 : (timeout - elapsed);
        }

        uint8_t item = 0;
        safe_event_t ev = EV_TIMEOUT;
        if (xQueueReceive(s_events, &item, wait) == pdPASS) {
            ev = (item < EV_COUNT) ? (safe_event_t)item : EV_TIMEOUT;
        }

        safe_state_t next;
        if (atomic_exchange(&s_event_lost, false) && state != SAFE_OPEN) {
            ESP_LOGW(TAG, "event lost, sequence broken");
            next = SAFE_LOCKED;
            press_armed = false;
        } else if ((ev == EV_SHORT || ev == EV_LONG) && !press_armed) {
            continue; // хвіст натискання, що почалось у відчиненому сейфі
        } else {
            if (ev == EV_PRESS) {
                press_armed = (state != SAFE_OPEN);
            } else if (ev == EV_SHORT || ev == EV_LONG) {
                press_armed = false;
            }
            next = safe_next(state, ev);
        }

        // Таймаут перезапускається при зміні стану і при будь-якій події під час вводу.
        // У SAFE_OPEN - лише на вході, натискання його не продовжують.
        const bool input_state = (next == SAFE_GOT_S || next == SAFE_GOT_SS);
        if (next != state || input_state) {
            entered_at = xTaskGetTickCount();
        }
        if (next != state) {
            log_transition(state, ev, next);
            state = next;
            apply_outputs(state);
        }
    }
}

// ================================== Старт ==================================

void app_main(void)
{
    // Вихід спершу в 0, потім у режим виходу: світлодіод "відчинено" не блимне при старті.
    ESP_ERROR_CHECK(gpio_set_level(LED_OPEN_PIN, 0));
    const gpio_config_t led_cfg = {
        .pin_bit_mask = 1ULL << LED_OPEN_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led_cfg));

    const gpio_config_t btn_cfg = {
        .pin_bit_mask = 1ULL << BUTTON_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, // без підтяжки вхід "плаває" і ловить наведення
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,   // опитування, переривання не потрібне
    };
    ESP_ERROR_CHECK(gpio_config(&btn_cfg));

    // Черга - до задач, які нею користуються. Статична: heap не потрібен, не може не вистачити.
    s_events = xQueueCreateStatic(EVENT_QUEUE_LEN, sizeof(uint8_t), s_queue_storage, &s_queue_struct);

    // Consumer першим: коли кнопка надішле подію, її вже є кому прийняти.
    if (xTaskCreate(safe_task, "safe", TASK_STACK_BYTES, NULL, SAFE_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "safe_task not created, safe stays locked");
        return;
    }
    if (xTaskCreate(button_task, "button", TASK_STACK_BYTES, NULL, BUTTON_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "button_task not created, safe stays locked");
        return;
    }
}
