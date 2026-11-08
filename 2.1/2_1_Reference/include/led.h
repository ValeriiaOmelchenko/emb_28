#pragma once
// «Драйвер» світлодіода: дані окремо (struct), поведінка окремо (функції).
// Жодних класів, віртуальних методів і динамічної памʼяті: struct Led займає 2 байти,
// а функції компілятор зазвичай вбудовує (inline) прямо в місце виклику.

#include <Arduino.h>
#include <stdint.h>

// enum class, а не bool чи int:
//   - ledSet(led, LedState::On) читається однозначно, ledSet(led, 1) - ні;
//   - не перетворюється неявно на int, тож ledSet(led, 42) не скомпілюється;
//   - : uint8_t - займає 1 байт.
enum class LedState : uint8_t { Off, On };

struct Led {
  uint8_t  pin;
  LedState state;  // памʼятаємо стан, щоб уміти перемикати без digitalRead() з виходу
};

// Ініціалізація периферії явно, у setup() (лекція, розділ 5: «The Init() Function»).
// Після init LED гарантовано вимкнений: відомий стан на старті.
inline void ledSet(Led& led, LedState state) {
  led.state = state;
  digitalWrite(led.pin, state == LedState::On ? HIGH : LOW);
}

inline void ledInit(Led& led) {
  pinMode(led.pin, OUTPUT);
  ledSet(led, LedState::Off);
}

inline void ledToggle(Led& led) {
  ledSet(led, led.state == LedState::On ? LedState::Off : LedState::On);
}
