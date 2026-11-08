# Embedded - група 28: практика та еталони

Код з уроків і еталонні рішення домашніх завдань. Плата курсу - ESP32-S3-DevKitC-1, середовище - VS Code + PlatformIO.

## Як користуватися

1. Клонуйте репозиторій у шлях латиницею без пробілів, наприклад `C:\esp\emb_28`. ESP-IDF не збирається, якщо в шляху є кирилиця або пробіли.
2. Нові матеріали з'являються після уроків: `git pull`.
3. Кожен проєкт (`M_L_Practice`, `M_L_Reference`) відкривайте у VS Code окремим вікном: File → Open Folder → папка з `platformio.ini`.
4. Експериментуйте в копії проєкту, щоб `git pull` не конфліктував з вашими змінами.

## Зміст

| Урок | Тема | Умова | Практика | Еталон |
|---|---|---|---|---|
| 1.2 | Світлодіоди, резистори, закон Ома | [Task](1.2/Task.md) | - | - |
| 1.3 | Знайомство з ESP32, Blink | [Task](1.3/Task.md) | - | [1_3_Reference](1.3/1_3_Reference/) |
| 1.4 | Кнопки, INPUT_PULLUP, BOOT | [Task](1.4/Task.md) | - | [1_4_Reference](1.4/1_4_Reference/) |
| 1.5 | Брязкіт контактів, переривання | [Task](1.5/Task.md) | - | - |
| 1.6 | АЦП і фоторезистор | [Task](1.6/Task.md) | [1_6_Practice](1.6/1_6_Practice/) | [1_6_Reference](1.6/1_6_Reference/) |
| 1.mini | Мініпроєкт: сутінковий вимикач | [Task](1.mini/Task.md) | - | [1_mini_Reference](1.mini/1_mini_Reference/) |
| ГС1 | Комп'ютерна логіка | - | [Практика](FS1_Computer_Logic/Практика.md) | - |
| 2.1 | Embedded C++ на ESP32 | [Task](2.1/Task.md) | [2_1_Practice](2.1/2_1_Practice/) | [2_1_Reference](2.1/2_1_Reference/) |
| 2.3 | Superloop та незалежні задачі | [Task](2.3/Task.md) | [2_3_Practice](2.3/2_3_Practice/) | [2_3_Reference](2.3/2_3_Reference/) |
| 6.1 | RTOS introduction | [Task](6.1/Task.md) | [6_1_Practice](6.1/6_1_Practice/) | - |
| 6.2 | FreeRTOS: задачі та планувальник | [Task](6.2/Task.md) | [6_2_Practice](6.2/6_2_Practice/), [6_2_SystemView](6.2/6_2_SystemView/) | - |
