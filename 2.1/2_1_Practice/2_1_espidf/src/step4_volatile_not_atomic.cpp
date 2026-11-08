// Крок 4. volatile ≠ atomic: два ядра ESP32-S3 рахують в один лічильник.
//
// Дві задачі (одна на ядрі 0, друга на ядрі 1) по ITERATIONS разів збільшують спільний лічильник.
// Очікуємо 2 × ITERATIONS. Три варіанти:
//   Volatile - volatile uint32_t, «x = x + 1»: читання, додавання, запис. Оновлення губляться.
//   Atomic   - std::atomic<uint32_t>::fetch_add: одна неподільна операція (інструкція S32C1I).
//   Critical - звичайна змінна під spinlock-ом portENTER_CRITICAL / portEXIT_CRITICAL.
// Натисни BOOT, щоб повторити експеримент.

#if PRACTICE_STEP == 4

#include <atomic>
#include <cinttypes>
#include <cstdio>

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

constexpr gpio_num_t  PIN_BOOT   = GPIO_NUM_0;
constexpr uint32_t    ITERATIONS = 1000000;
constexpr uint32_t    TASK_STACK = 2048;  // байт
constexpr UBaseType_t TASK_PRIO  = 2;     // вище за задачу main (1)

enum class Mode : uint8_t { Volatile, Atomic, Critical };
constexpr Mode MODES[] = {Mode::Volatile, Mode::Atomic, Mode::Critical};

static volatile uint32_t volatileCounter = 0;
static std::atomic<uint32_t> atomicCounter{0};
static uint32_t criticalCounter = 0;
static portMUX_TYPE counterMux = portMUX_INITIALIZER_UNLOCKED;

static std::atomic<uint8_t> finishedWorkers{0};

static const char* modeName(Mode mode) {
  switch (mode) {
    case Mode::Volatile: return "volatile";
    case Mode::Atomic:   return "std::atomic";
    case Mode::Critical: return "critical section";
  }
  return "?";
}

static void worker(void* param) {
  const Mode mode = *static_cast<const Mode*>(param);

  for (uint32_t i = 0; i < ITERATIONS; ++i) {
    switch (mode) {
      case Mode::Volatile:
        volatileCounter = volatileCounter + 1;  // між читанням і записом друге ядро встигає своє
        break;
      case Mode::Atomic:
        // relaxed: потрібна лише неподільність, порядок відносно інших змінних неважливий
        atomicCounter.fetch_add(1, std::memory_order_relaxed);
        break;
      case Mode::Critical:
        portENTER_CRITICAL(&counterMux);
        ++criticalCounter;
        portEXIT_CRITICAL(&counterMux);
        break;
    }
  }

  finishedWorkers.fetch_add(1);
  vTaskDelete(nullptr);  // з функції задачі не можна просто вийти через return
}

static uint32_t readCounter(Mode mode) {
  switch (mode) {
    case Mode::Volatile: return volatileCounter;
    case Mode::Atomic:   return atomicCounter.load();
    case Mode::Critical: return criticalCounter;
  }
  return 0;
}

static void runExperiment(const Mode& mode) {
  volatileCounter = 0;
  atomicCounter.store(0);
  criticalCounter = 0;
  finishedWorkers.store(0);

  const int64_t startUs = esp_timer_get_time();

  // Спершу ядро 1: app_main() працює на ядрі 0, і задача з вищим пріоритетом на ядрі 0
  // одразу витіснить app_main(). Так обидві задачі стартують майже одночасно.
  void* param = const_cast<Mode*>(&mode);
  xTaskCreatePinnedToCore(worker, "worker1", TASK_STACK, param, TASK_PRIO, nullptr, 1);
  xTaskCreatePinnedToCore(worker, "worker0", TASK_STACK, param, TASK_PRIO, nullptr, 0);

  while (finishedWorkers.load() < 2) {
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  const uint32_t elapsedUs = static_cast<uint32_t>(esp_timer_get_time() - startUs);
  const uint32_t expected = 2 * ITERATIONS;
  const uint32_t actual = readCounter(mode);

  printf("%-17s очікувано %" PRIu32 ", отримано %" PRIu32 ", втрачено %" PRIu32 ", час %" PRIu32 " мкс\n",
         modeName(mode), expected, actual, expected - actual, elapsedUs);
}

static void runAll() {
  printf("\n=== Крок 4: volatile vs atomic vs critical section ===\n");
  for (const Mode& mode : MODES) {
    runExperiment(mode);
  }
  printf("Натисни BOOT, щоб повторити\n");
}

extern "C" void app_main() {
  const gpio_config_t config = {
      .pin_bit_mask = 1ULL << PIN_BOOT,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,  // тут ISR не потрібна: опитування цілком достатньо
  };
  ESP_ERROR_CHECK(gpio_config(&config));

  runAll();

  while (true) {
    if (gpio_get_level(PIN_BOOT) == 0) {
      runAll();
      while (gpio_get_level(PIN_BOOT) == 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
      }
      vTaskDelay(pdMS_TO_TICKS(50));  // проста пауза від брязкоту при відпусканні
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

#endif  // PRACTICE_STEP == 4
