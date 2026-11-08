#pragma once
// Усі параметри прошивки в одному місці. Жодного «магічного числа» в main.cpp.
// namespace + constexpr, а не #define: є тип, область видимості (config::...),
// значення відоме на етапі компіляції і не займає RAM.

#include <stdint.h>

namespace config {

// --- Залізо (ESP32-S3-DevKitC-1) ---
// GPIO 4-6: не strapping (0, 3, 45, 46), не USB (19, 20), не UART0-консоль (43, 44),
// не флеш/PSRAM (26-37).
constexpr uint8_t LED_RED_PIN   = 4;
constexpr uint8_t LED_GREEN_PIN = 5;
constexpr uint8_t LED_BLUE_PIN  = 6;

// --- Таймінги ---
// Інтервал = час між перемиканнями (півперіод): 200 мс горить, 200 мс ні.
constexpr uint32_t RED_TOGGLE_MS   = 200;
constexpr uint32_t GREEN_TOGGLE_MS = 500;
constexpr uint32_t BLUE_TOGGLE_MS  = 1000;

// --- Контрольний звіт ---
// Раз на 10 с друкуємо, скільки разів перемкнувся кожен LED за вікно.
// Очікування: 10000 / 200 = 50, 10000 / 500 = 20, 10000 / 1000 = 10.
constexpr uint32_t REPORT_PERIOD_MS = 10000;
constexpr uint32_t SERIAL_BAUD      = 115200;

// --- Перевірки на етапі компіляції ---
// Нульовий інтервал = LED перемикається на кожному проході loop() і виглядає
// тьмяно ввімкненим. Хай це ловить компілятор, а не плата.
static_assert(RED_TOGGLE_MS > 0 && GREEN_TOGGLE_MS > 0 && BLUE_TOGGLE_MS > 0,
              "toggle interval must be > 0");
static_assert(REPORT_PERIOD_MS > 0, "REPORT_PERIOD_MS must be > 0");
static_assert(LED_RED_PIN != LED_GREEN_PIN && LED_RED_PIN != LED_BLUE_PIN &&
                  LED_GREEN_PIN != LED_BLUE_PIN,
              "each LED needs its own pin");

}  // namespace config
