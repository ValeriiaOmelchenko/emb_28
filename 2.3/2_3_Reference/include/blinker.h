#pragma once
// Неблокуючий «блимач»: дані окремо (struct), поведінка окремо (функції).
// Без класів, віртуальних методів і динамічної памʼяті.

#include <Arduino.h>
#include <stdint.h>

struct Blinker {
  const char* name;          // рядок-літерал у флеші, для звіту
  uint8_t     pin;
  uint32_t    toggleMs;      // інтервал між перемиканнями
  uint32_t    lastToggleMs;  // «вузол сітки» часу, від якого рахуємо наступне перемикання
  uint32_t    toggles;       // лічильник для контрольного звіту
  bool        isOn;          // стан живе поруч із піном: одне джерело правди
};

// Ініціалізація явно, у setup(): режим піна, відомий рівень на старті, точка відліку.
inline void blinkerInit(Blinker& b, uint32_t nowMs) {
  pinMode(b.pin, OUTPUT);
  digitalWrite(b.pin, LOW);
  b.isOn         = false;
  b.lastToggleMs = nowMs;
  b.toggles      = 0;
}

// Викликається на кожному проході loop(). Нічого не чекає: перевірила «чи настав час?»
// і одразу повернулась.
inline void blinkerUpdate(Blinker& b, uint32_t nowMs) {
  // Беззнакове віднімання: коректне і через переповнення millis() (~49,7 доби).
  if (nowMs - b.lastToggleMs < b.toggleMs) {
    return;
  }

  // Фіксована сітка часу: наступне перемикання рахуємо від запланованого моменту,
  // а не від фактичного. Запізнення проходу loop() не накопичується.
  b.lastToggleMs += b.toggleMs;
  // Якщо loop() заблокувався довше за інтервал, не «наздоганяємо» пропущене пачкою
  // перемикань, а починаємо сітку заново від поточного моменту.
  if (nowMs - b.lastToggleMs >= b.toggleMs) {
    b.lastToggleMs = nowMs;
  }

  b.isOn = !b.isOn;
  digitalWrite(b.pin, b.isOn ? HIGH : LOW);
  b.toggles++;
}
