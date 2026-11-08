// Крок 5. «Розірване» читання 64-бітної змінної на 32-бітному ядрі.
//
// Задача-писар на ядрі 0 записує в спільну змінну значення, у якого старша і молодша
// половини однакові: (i << 32) | i. Читач у loop() на ядрі 1 перевіряє, чи половини збігаються.
// Якщо ні - він прочитав половину старого і половину нового значення (torn read).
//   Volatile - volatile uint64_t: два 32-бітні записи і два 32-бітні читання. Розриви є.
//   Critical - читання і запис під spinlock-ом. Розривів немає.
//   Atomic   - std::atomic<uint64_t>: розривів немає, але is_lock_free() = false:
//              на Xtensa немає 64-бітних атомарних інструкцій, усередині прихований замок.
// Натисни BOOT, щоб повторити.

#include <Arduino.h>
#include <atomic>

constexpr uint8_t  PIN_BOOT      = 0;
constexpr uint32_t WRITES        = 2000000;
constexpr uint32_t TASK_STACK    = 2048;
constexpr UBaseType_t TASK_PRIO  = 1;

enum class Mode : uint8_t { Volatile, Critical, Atomic };
constexpr Mode MODES[] = {Mode::Volatile, Mode::Critical, Mode::Atomic};

static volatile uint64_t volatileValue = 0;
static uint64_t criticalValue = 0;
static std::atomic<uint64_t> atomicValue{0};
static portMUX_TYPE valueMux = portMUX_INITIALIZER_UNLOCKED;

static std::atomic<bool> writerDone{false};

static const char* modeName(Mode mode) {
  switch (mode) {
    case Mode::Volatile: return "volatile uint64_t";
    case Mode::Critical: return "critical section";
    case Mode::Atomic:   return "std::atomic<u64>";
  }
  return "?";
}

static constexpr uint64_t makePattern(uint32_t i) {
  return (static_cast<uint64_t>(i) << 32) | i;
}

static void writer(void* param) {
  const Mode mode = *static_cast<const Mode*>(param);

  for (uint32_t i = 1; i <= WRITES; ++i) {
    const uint64_t value = makePattern(i);
    switch (mode) {
      case Mode::Volatile:
        volatileValue = value;
        break;
      case Mode::Critical:
        portENTER_CRITICAL(&valueMux);
        criticalValue = value;
        portEXIT_CRITICAL(&valueMux);
        break;
      case Mode::Atomic:
        atomicValue.store(value);
        break;
    }
  }

  writerDone.store(true);
  vTaskDelete(nullptr);
}

static uint64_t readValue(Mode mode) {
  switch (mode) {
    case Mode::Volatile:
      return volatileValue;
    case Mode::Critical: {
      portENTER_CRITICAL(&valueMux);
      const uint64_t value = criticalValue;
      portEXIT_CRITICAL(&valueMux);
      return value;
    }
    case Mode::Atomic:
      return atomicValue.load();
  }
  return 0;
}

static void runExperiment(const Mode& mode) {
  volatileValue = 0;
  criticalValue = 0;
  atomicValue.store(0);
  writerDone.store(false);

  xTaskCreatePinnedToCore(writer, "writer", TASK_STACK, const_cast<Mode*>(&mode), TASK_PRIO, nullptr, 0);

  uint32_t reads = 0;
  uint32_t torn = 0;
  while (!writerDone.load()) {
    const uint64_t value = readValue(mode);
    const uint32_t high = static_cast<uint32_t>(value >> 32);
    const uint32_t low = static_cast<uint32_t>(value);
    if (high != low) {
      ++torn;
    }
    ++reads;
  }

  Serial.printf("%-18s читань %u, розірваних %u\n",
                modeName(mode), static_cast<unsigned>(reads), static_cast<unsigned>(torn));
}

static void runAll() {
  Serial.println("\n=== Крок 5: torn read 64-бітної змінної ===");
  // Стандартні макроси: 2 - завжди lock-free, 1 - інколи, 0 - ніколи (усередині замок).
  // atomicValue.is_lock_free() тут навіть не злінкується: для 64 біт компілятор
  // кличе бібліотечну __atomic_is_lock_free, якої в Arduino-ESP32 немає.
  Serial.printf("lock-free: int (32 біт) = %d, long long (64 біт) = %d\n",
                ATOMIC_INT_LOCK_FREE, ATOMIC_LLONG_LOCK_FREE);
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
  if (digitalRead(PIN_BOOT) == LOW) {
    runAll();
    while (digitalRead(PIN_BOOT) == LOW) {
      delay(10);
    }
    delay(50);
  }
  delay(10);
}
