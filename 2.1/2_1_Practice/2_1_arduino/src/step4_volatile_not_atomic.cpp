// Крок 4. volatile ≠ atomic: два ядра ESP32-S3 рахують в один лічильник.
//
// Дві задачі (одна на ядрі 0, друга на ядрі 1) по ITERATIONS разів збільшують спільний лічильник.
// Очікуємо 2 × ITERATIONS. Три варіанти:
//   Volatile - volatile uint32_t, «x = x + 1»: читання, додавання, запис. Оновлення губляться.
//   Atomic   - std::atomic<uint32_t>::fetch_add: одна неподільна операція (інструкція S32C1I).
//   Critical - звичайна змінна під spinlock-ом portENTER_CRITICAL / portEXIT_CRITICAL.
// Натисни BOOT, щоб повторити експеримент.

#include <Arduino.h>
#include <atomic>

constexpr uint8_t  PIN_BOOT   = 0;
constexpr uint32_t ITERATIONS = 1000000;
constexpr uint32_t TASK_STACK = 2048;  // байт
constexpr UBaseType_t TASK_PRIO = 2;

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

  const uint32_t startUs = micros();

  // Спершу ядро 0: loop() працює на ядрі 1, і задача з вищим пріоритетом на ядрі 1
  // одразу витіснить loop(). Так обидві задачі стартують майже одночасно.
  xTaskCreatePinnedToCore(worker, "worker0", TASK_STACK, const_cast<Mode*>(&mode), TASK_PRIO, nullptr, 0);
  xTaskCreatePinnedToCore(worker, "worker1", TASK_STACK, const_cast<Mode*>(&mode), TASK_PRIO, nullptr, 1);

  while (finishedWorkers.load() < 2) {
    delay(1);
  }

  const uint32_t elapsedUs = micros() - startUs;
  const uint32_t expected = 2 * ITERATIONS;
  const uint32_t actual = readCounter(mode);

  Serial.printf("%-17s очікувано %u, отримано %u, втрачено %u, час %u мкс\n",
                modeName(mode),
                static_cast<unsigned>(expected),
                static_cast<unsigned>(actual),
                static_cast<unsigned>(expected - actual),
                static_cast<unsigned>(elapsedUs));
}

static void runAll() {
  Serial.println("\n=== Крок 4: volatile vs atomic vs critical section ===");
  for (const Mode& mode : MODES) {
    runExperiment(mode);
  }
  Serial.println("Натисни BOOT, щоб повторити");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  runAll();
}

void loop() {
  // Тут ISR не потрібна: опитування кнопки цілком достатньо для «запустити ще раз».
  if (digitalRead(PIN_BOOT) == LOW) {
    runAll();
    while (digitalRead(PIN_BOOT) == LOW) {
      delay(10);
    }
    delay(50);  // проста пауза від брязкоту при відпусканні
  }
  delay(10);
}
