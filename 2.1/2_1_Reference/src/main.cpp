// Еталон 2.1 - Embedded C++ на ESP32-S3 (Arduino), процедурний стиль.
//
// Superloop із трьох неблокуючих кроків:
//   1) pollButtonPress()  - подія натискання: ISR лише ставить прапор, а loop() після
//                           першого фронту ВІДКЛЮЧАЄ переривання, щоб брязкіт не
//                           викликав ISR десятки разів («шторм переривань»);
//   2) updateLed()        - режими Блимання → Увімкнено → Вимкнено → Блимання;
//   3) updateLoopStats()  - середній і максимальний час ітерації loop().
// Жодного delay(), new/malloc, String, STL-контейнерів.

#include <Arduino.h>
#include "config.h"
#include "led.h"

enum class LedMode : uint8_t { Blinking, On, Off };

// Стани кнопки. Кухар на кухні: почув дзвінок (Armed) - вимкнув дзвінок і пішов глянути,
// чи справді хтось прийшов (Confirming) - дочекався, поки гість зайде і двері зачиняться
// (WaitRelease) - знову ввімкнув дзвінок.
enum class ButtonState : uint8_t {
  Armed,        // переривання підключене, чекаємо першого фронту
  Confirming,   // переривання відключене, чекаємо PRESS_LOCKOUT_MS і читаємо рівень
  WaitRelease,  // переривання відключене, чекаємо стабільного відпускання
};

// ---------------------------------------------------------------------------
// Глобальні змінні: лише ті, що потрібні кільком функціям.
// ---------------------------------------------------------------------------

// LED потрібен і setup() (ініціалізація), і loop() (керування).
// Aggregate-ініціалізація константами → змінна в секції .data: початкові значення
// копіюються з флешу в RAM стартовим кодом ще до setup(), без жодного конструктора.
static Led led = {config::LED_PIN, LedState::Off};

// Пише ISR, читає loop() → volatile: без нього компілятор має право прочитати
// змінну один раз і тримати копію в регістрі.
// bool і uint32_t читаються/пишуться однією інструкцією на 32-бітному ядрі.
static volatile bool     buttonEdge = false;  // «був спадний фронт»
static volatile uint32_t isrCalls   = 0;      // для демо: скільки разів викликалась ISR

// ---------------------------------------------------------------------------
// ISR: мінімальна. Ні Serial, ні затримок, ні detachInterrupt() - лише прапор.
// IRAM_ATTR: код ISR лежить у внутрішній RAM і спрацює навіть тоді, коли
// флеш зайнятий (запис NVS, OTA). Інакше - виняток Cache disabled.
// ---------------------------------------------------------------------------
static void IRAM_ATTR onButtonFalling() {
  buttonEdge = true;
  isrCalls = isrCalls + 1;  // пише лише ISR, тому без гонки; не «++», бо volatile
}

static void armButtonInterrupt() {
  buttonEdge = false;  // фронти, що встигли прийти до відключення, - старий брязкіт
  attachInterrupt(digitalPinToInterrupt(config::BUTTON_PIN), onButtonFalling, FALLING);
}

// ---------------------------------------------------------------------------
// Кнопка: антибрязкіт через тимчасове відключення переривання.
//
// Без цього кожен «стрибок» контакту викликає ISR: одне натискання = 5-20 переривань
// (урок 1.5), а на зношеній кнопці або з повільним фронтом - сотні. Кожне переривання
// забирає процесор у loop(). Тому після першого фронту переривання вимикаємо, і далі
// брязкіт процесор взагалі не турбує.
//
// Чому не просто «вимкнути на 30 мс і ввімкнути назад»: якщо тримати кнопку довше,
// брязкіт ВІДПУСКАННЯ дасть спадні фронти, і ISR сприйме їх як нове натискання.
// Тому переривання повертаємо, лише коли кнопка стабільно відпущена.
//
// Повертає true рівно один раз на кожне натискання.
// ---------------------------------------------------------------------------
static bool pollButtonPress(uint32_t nowMs) {
  static ButtonState state     = ButtonState::Armed;
  static uint32_t    stateSinceMs = 0;

  switch (state) {
    case ButtonState::Armed:
      if (buttonEdge) {
        // Відключаємо в loop(), а не в ISR: ISR лишається мінімальною, а за ті мікросекунди,
        // поки loop() дійде сюди, проскочить хіба кілька зайвих викликів ISR, а не десятки.
        detachInterrupt(digitalPinToInterrupt(config::BUTTON_PIN));
        stateSinceMs = nowMs;
        state = ButtonState::Confirming;
      }
      return false;

    case ButtonState::Confirming:
      if (nowMs - stateSinceMs < config::PRESS_LOCKOUT_MS) {
        return false;  // брязкіт натискання ще триває, переривання вимкнене
      }
      stateSinceMs = nowMs;
      state = ButtonState::WaitRelease;
      // Брязкіт закінчився - рівню можна вірити. HIGH означає, що це була завада, а не кнопка
      // (Trust No Pin: наводка на довгому дроті теж дає «фронт»).
      return digitalRead(config::BUTTON_PIN) == LOW;  // INPUT_PULLUP: натиснута = LOW

    case ButtonState::WaitRelease:
      // Polling рівня: кожен LOW (кнопку ще тримають або брязкіт відпускання)
      // перезапускає вікно тиші.
      if (digitalRead(config::BUTTON_PIN) == LOW) {
        stateSinceMs = nowMs;
      } else if (nowMs - stateSinceMs >= config::RELEASE_QUIET_MS) {
        armButtonInterrupt();
        state = ButtonState::Armed;
      }
      return false;
  }
  return false;
}

static LedMode nextMode(LedMode mode) {
  switch (mode) {
    case LedMode::Blinking: return LedMode::On;
    case LedMode::On:       return LedMode::Off;
    case LedMode::Off:      return LedMode::Blinking;
  }
  return LedMode::Blinking;  // недосяжно, але компілятор вимагає повернення
}

static const char* modeName(LedMode mode) {
  switch (mode) {
    case LedMode::Blinking: return "Блимання";
    case LedMode::On:       return "Постійно увімкнено";
    case LedMode::Off:      return "Постійно вимкнено";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// LED: неблокуючий blink на millis().
// ---------------------------------------------------------------------------
static void updateLed(LedMode mode, bool modeChanged, uint32_t nowMs) {
  static uint32_t lastToggleMs = 0;

  if (modeChanged) {
    // Одразу виставляємо вихід під новий режим, щоб LED не «залип» у старому стані.
    // Блимання починаємо з On: користувач одразу бачить реакцію на кнопку.
    ledSet(led, mode == LedMode::Off ? LedState::Off : LedState::On);
    lastToggleMs = nowMs;
    return;
  }

  // Беззнакове віднімання: коректне і через переповнення millis() (~49,7 доби).
  if (mode == LedMode::Blinking && nowMs - lastToggleMs >= config::BLINK_INTERVAL_MS) {
    lastToggleMs = nowMs;
    ledToggle(led);
  }
}

// ---------------------------------------------------------------------------
// Вимірювання superloop (опційний пункт 3).
//   середнє = час блоку з LOOP_STATS_ITERATIONS ітерацій / кількість
//            (точніше, ніж міряти кожну ітерацію: micros() має крок 1 мкс,
//             а ітерація може бути коротшою);
//   максимум = найдовша окрема ітерація: саме вона визначає найгіршу реакцію.
// ---------------------------------------------------------------------------
static void updateLoopStats(uint32_t nowMs) {
  static uint32_t blockStartUs = micros();
  static uint32_t prevUs       = blockStartUs;
  static uint32_t maxUs        = 0;
  static uint32_t iterations   = 0;
  static uint32_t lastPrintMs  = 0;

  const uint32_t nowUs = micros();
  const uint32_t iterUs = nowUs - prevUs;  // беззнаково: micros() переповнюється раз на ~71 хв
  prevUs = nowUs;
  if (iterUs > maxUs) {
    maxUs = iterUs;
  }

  iterations++;
  if (iterations < config::LOOP_STATS_ITERATIONS) {
    return;
  }

  if (nowMs - lastPrintMs >= config::STATS_PRINT_PERIOD_MS) {
    lastPrintMs = nowMs;
    const uint32_t blockUs = nowUs - blockStartUs;
    // ×1000 у 64 бітах: у 32 бітах переповниться, якщо блок довший за ~4,3 с.
    const uint32_t avgNs = static_cast<uint32_t>(
        static_cast<uint64_t>(blockUs) * 1000u / config::LOOP_STATS_ITERATIONS);
    Serial.printf("loop(): середнє %u нс, макс %u мкс (блок %u ітерацій)\n",
                  static_cast<unsigned>(avgNs),
                  static_cast<unsigned>(maxUs),
                  static_cast<unsigned>(config::LOOP_STATS_ITERATIONS));
    prevUs = micros();  // час самого Serial.printf не входить у наступний блок
  }

  blockStartUs = prevUs;
  iterations = 0;
  maxUs = 0;
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(config::SERIAL_BAUD);

  ledInit(led);                                   // відомий стан: LED вимкнений
  pinMode(config::BUTTON_PIN, INPUT_PULLUP);      // без підтяжки пін «плаває»
  armButtonInterrupt();

  Serial.println("Еталон 2.1: кнопка перемикає Блимання → Увімкнено → Вимкнено");
}

void loop() {
  static LedMode  mode = LedMode::Blinking;
  static uint32_t isrCallsShown = 0;

  const uint32_t nowMs = millis();  // один знімок часу на весь прохід

  const bool pressed = pollButtonPress(nowMs);
  if (pressed) {
    mode = nextMode(mode);
    // Знімок volatile один раз: читаємо в локальну змінну і далі працюємо з копією.
    const uint32_t calls = isrCalls;
    // Друк лише на подію, не щоітерації.
    Serial.printf("Режим: %s (викликів ISR від минулого натискання: %u)\n",
                  modeName(mode), static_cast<unsigned>(calls - isrCallsShown));
    isrCallsShown = calls;
  }

  updateLed(mode, pressed, nowMs);
  updateLoopStats(nowMs);
}
