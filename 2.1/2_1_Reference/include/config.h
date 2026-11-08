#pragma once
// Усі параметри прошивки в одному місці. Жодного «магічного числа» в main.cpp.
//
// namespace + constexpr, а не #define:
//   - має тип (uint8_t, uint32_t) → компілятор перевіряє діапазон і знак;
//   - має область видимості (config::LED_PIN) → не конфліктує з чужими макросами;
//   - видно в дебагері;
//   - обчислюється на етапі компіляції → не займає RAM і не рахується в runtime.
// constexpr на рівні namespace має внутрішнє звʼязування: можна включати
// цей файл у кілька .cpp без помилки «multiple definition».

#include <stdint.h>

namespace config {

// --- Залізо (ESP32-S3-DevKitC-1) ---
// GPIO 4 і 7: не strapping (0, 3, 45, 46), не USB (19, 20), не флеш/PSRAM (26-37).
constexpr uint8_t LED_PIN    = 4;   // зовнішній LED через резистор на GND
constexpr uint8_t BUTTON_PIN = 7;   // кнопка на GND, внутрішня підтяжка → натиснута = LOW

// --- Таймінги ---
constexpr uint32_t BLINK_INTERVAL_MS = 500;   // півперіод: 500 мс горить, 500 мс ні
// Антибрязкіт через відключення переривання. Обидва часи = 2-3 × МАКСИМАЛЬНОГО
// брязкоту (урок 1.5: у більшості кнопок до ~6-10 мс), а не середнього.
// Після першого фронту переривання вимкнене PRESS_LOCKOUT_MS, потім читаємо рівень:
// LOW - натискання підтверджене, HIGH - це була завада.
constexpr uint32_t PRESS_LOCKOUT_MS  = 30;
// Переривання повертаємо, лише коли кнопка стабільно відпущена стільки часу.
// Інакше брязкіт ВІДПУСКАННЯ дасть спадні фронти і хибне натискання.
constexpr uint32_t RELEASE_QUIET_MS  = 30;

// --- Вимірювання superloop ---
constexpr uint32_t LOOP_STATS_ITERATIONS = 1000;  // розмір блоку усереднення (з умови)
// Блок із 1000 ітерацій проходить за ~мілісекунди. Друкувати кожен = сотні рядків
// за секунду, і сам Serial почне гальмувати loop(). Тому друкуємо не частіше за це:
constexpr uint32_t STATS_PRINT_PERIOD_MS = 1000;

// --- Serial ---
constexpr uint32_t SERIAL_BAUD = 115200;

// --- Перевірки на етапі компіляції ---
// Нульовий період = таймер «спрацьовує» на кожному проході loop(), і LED перемикається
// сотні тисяч разів на секунду (виглядає як тьмяне світіння, а не блимання).
// Нуль ітерацій = ділення на нуль у середньому часі. Хай це ловить компілятор, а не плата.
static_assert(BLINK_INTERVAL_MS > 0,     "BLINK_INTERVAL_MS must be > 0");
static_assert(PRESS_LOCKOUT_MS > 0,      "PRESS_LOCKOUT_MS must be > 0");
static_assert(RELEASE_QUIET_MS > 0,      "RELEASE_QUIET_MS must be > 0");
static_assert(LOOP_STATS_ITERATIONS > 0, "LOOP_STATS_ITERATIONS must be > 0");
static_assert(LED_PIN != BUTTON_PIN,     "LED and button must use different pins");

}  // namespace config
