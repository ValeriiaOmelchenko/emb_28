#pragma once
// Спільне для всіх кроків практики 6.4: консоль як джерело подій, стеки, пріоритети.
// Чистий C (C17, стандарт ESP-IDF за замовчуванням): процедурно, без динамічної памʼяті в циклі.
//
// Константи - #define з явним типом: у C const-змінна не є константним виразом
// (не годиться для розміру масиву, static_assert і ініціалізатора static-таблиці).

#include <stdint.h>
#include <stdlib.h>

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#if !CONFIG_ESP_CONSOLE_UART
#error "Практика читає клавіші з UART-консолі: menuconfig -> Component config -> ESP System Settings -> Channel for console output = Default: UART0"
#endif

// --- Стеки задач (в ESP-IDF - у БАЙТАХ) ---
// ESP_LOGx сам по собі зʼїдає 1,5-2 КБ стеку, тому для задач з логом - 3 КБ.
#define LOG_TASK_STACK_BYTES UINT32_C(3072)

// --- Пріоритети (більше число = важливіше). app_main працює з пріоритетом 1. ---
// Автомат важливіший за джерело подій: подія, що прийшла, обробляється одразу.
#define PRIO_CONSOLE ((UBaseType_t)4)
#define PRIO_FSM     ((UBaseType_t)5)

// --- Черга подій ---
// Автомат не знає, звідки беруться події (консоль, кнопка, MQTT): він лише читає чергу.
#define EVENT_QUEUE_LENGTH ((UBaseType_t)8)

// --- Консоль (UART0 через USB-UART міст плати) ---
#define CONSOLE_UART        ((uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM)
#define CONSOLE_RX_BUF_SIZE ((int)256) // байтів, тип - як у uart_driver_install(); драйвер вимагає більше за апаратний FIFO (128)

// Встановлює драйвер UART на консольний порт, щоб читати клавіші блокуючим викликом
// (задача спить у Blocked, поки нічого не натиснуто). Вивід ESP_LOG працює як і раніше.
static inline void console_init(void) {
  ESP_ERROR_CHECK(uart_driver_install(CONSOLE_UART, CONSOLE_RX_BUF_SIZE, 0, 0, NULL, 0));
}

// Чекає одну натиснуту клавішу. Задача спить, поки байт не прийде.
static inline char console_read_key(void) {
  uint8_t key = 0;
  while (uart_read_bytes(CONSOLE_UART, &key, 1, portMAX_DELAY) != 1) {
  }
  return (char)key;
}

// xTaskCreate() повертає pdPASS або помилку (не вистачило heap для стеку).
// Без перевірки «задача мовчить» не відрізнити від «задача не створилась».
static inline void check_task_created(BaseType_t result, const char *name) {
  if (result != pdPASS) {
    ESP_LOGE("practice", "Не вдалося створити задачу %s (мало heap?)", name);
    abort();
  }
}
