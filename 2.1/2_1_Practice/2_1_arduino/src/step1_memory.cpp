// Крок 1. const vs constexpr: де фізично лежать дані.
//
// Друкуємо адреси змінних і за діапазоном адрес визначаємо, де вони:
//   0x3C00_0000...0x3E00_0000 - Flash (DROM, секція .rodata, через кеш);
//   0x3FC8_8000...0x3FD0_0000 - внутрішня RAM (DRAM).
// Висновок лекції: const - про логічну незмінність, constexpr - про розміщення і обчислення на етапі компіляції.

#include <Arduino.h>
#include "soc/soc_memory_types.h"  // esp_ptr_in_drom(), esp_ptr_in_dram() (ESP-IDF 4.4 під Arduino 2.x)

// Значення відоме компілятору → таблиця лягає у Flash і не займає RAM.
constexpr uint16_t SINE_TABLE[8] = {0, 1567, 2896, 3784, 4095, 3784, 2896, 1567};

// const, але значення стане відомим лише після старту → RAM (.bss + ініціалізація до setup()).
static const uint32_t BOOT_MARK_CYCLES = ESP.getCycleCount();

// Звичайна змінна з ініціалізатором → RAM (.data: копіюється з Flash у RAM при старті).
static uint16_t workBuffer[8] = {1, 2, 3, 4, 5, 6, 7, 8};

// Без ініціалізатора → RAM (.bss: просто обнуляється при старті, у Flash не займає місця).
static uint8_t zeroBuffer[64];

static const char* regionOf(const void* p) {
  if (esp_ptr_in_drom(p)) {
    return "Flash (DROM)";
  }
  if (esp_ptr_in_dram(p)) {
    return "RAM (DRAM)";
  }
  return "інше";
}

static void printPlace(const char* name, const void* p) {
  Serial.printf("%-22s @ %p -> %s\n", name, p, regionOf(p));
}

void setup() {
  Serial.begin(115200);
  delay(500);

  uint8_t local[4] = {};  // локальна змінна живе на стеку задачі loopTask, а стек - у RAM

  Serial.println("\n=== Крок 1: де лежать дані ===");
  printPlace("constexpr SINE_TABLE", SINE_TABLE);
  printPlace("const BOOT_MARK_CYCLES", &BOOT_MARK_CYCLES);
  printPlace("workBuffer", workBuffer);
  printPlace("zeroBuffer", zeroBuffer);
  printPlace("рядок \"hello\"", "hello");
  printPlace("local (стек)", local);

  // Купа: скільки вільно і який найбільший шматок можна виділити одним malloc.
  // Різниця між цими числами - це фрагментація, через яку в циклі не роблять new/malloc/String.
  Serial.printf("Купа: вільно %u байт, найбільший блок %u байт\n",
                static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getMaxAllocHeap()));

  // Використовуємо дані, щоб компілятор не викинув їх як непотрібні.
  Serial.printf("(SINE_TABLE[2]=%u BOOT_MARK_CYCLES=%u workBuffer[0]=%u zeroBuffer[0]=%u local[0]=%u)\n",
                SINE_TABLE[2], static_cast<unsigned>(BOOT_MARK_CYCLES), workBuffer[0], zeroBuffer[0], local[0]);
}

void loop() {}
