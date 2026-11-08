# SystemView для своїх FreeRTOS-проєктів (ESP32-S3, PlatformIO)

SEGGER SystemView показує роботу FreeRTOS на часовій шкалі: яка задача зараз виконується і на якому ядрі, хто кого витіснив, хто чекає, скільки триває переривання.

Це бонус, а не обов'язкова частина домашки. Для здачі достатньо логів у консолі.

Аналогія: лог у консолі - це записки кухаря «зробив салат», «зробив суп». SystemView - відео з камери над кухнею: видно, хто кого відштовхнув від плити і хто так і не дочекався своєї черги.

## Що потрібно

- ESP32-S3-DevKitC-1 і кабель у порт USB (не UART). Через цей порт працюють і прошивка, і монітор, і трасування.
- Проєкт на ESP-IDF у PlatformIO (`framework = espidf`). З фреймворком Arduino ця інструкція не працює.
- J-Link не потрібен: дані йдуть через вбудований у ESP32-S3 USB-JTAG.

## Крок 1. Підготовка комп'ютера (один раз, зробіть до заняття)

### 1.1. SystemView

Завантажте і встановіть SystemView 3.60 або новіший: https://www.segger.com/downloads/systemview/

Для навчання він безкоштовний. Старіші версії не вміють відкривати запис з обох ядер в одному файлі.

### 1.2. Опис подій ESP-IDF

ESP-IDF нумерує події FreeRTOS не так, як стандартний FreeRTOS. Без цього кроку частина подій на шкалі буде без назв.

1. У папці SystemView відкрийте `Description\` і перейменуйте `SYSVIEW_FreeRTOS.txt` на `SYSVIEW_FreeRTOS.txt.orig`.
2. Скопіюйте туди файл
   `%USERPROFILE%\.platformio\packages\framework-espidf\tools\esp_app_trace\SYSVIEW_FreeRTOS.txt`

### 1.3. OpenOCD

OpenOCD - програма, яка читає дані з плати через JTAG. Потрібна саме версія `v0.12.0-esp32-20251215`, яку рекомендує ESP-IDF 5.5. Ту, що ставить сам PlatformIO (2022 року), не використовуйте: SystemView не відкриє її запис (помилка 140).

1. Завантажте: https://github.com/espressif/openocd-esp32/releases/download/v0.12.0-esp32-20251215/openocd-esp32-win64-0.12.0-esp32-20251215.zip
2. Розпакуйте в PowerShell:
   ```powershell
   Expand-Archive "$env:USERPROFILE\Downloads\openocd-esp32-win64-0.12.0-esp32-20251215.zip" "$env:USERPROFILE\.espressif\tools\openocd-esp32\v0.12.0-esp32-20251215"
   ```
3. Перевірте, що з'явився файл
   `%USERPROFILE%\.espressif\tools\openocd-esp32\v0.12.0-esp32-20251215\openocd-esp32\bin\openocd.exe`

### 1.4. Драйвер USB-JTAG (Windows)

1. Підключіть плату кабелем у порт USB.
2. Завантажте Zadig: https://zadig.akeo.ie (попросить права адміністратора).
3. Options > List All Devices.
4. Оберіть `USB JTAG/serial debug unit (Interface 2)`.
5. Праворуч оберіть `WinUSB` і натисніть Replace Driver.

Робіть це навіть тоді, коли там уже написано WinUSB: драйвер, який ставить сама Windows, OpenOCD відкрити не може.

`Interface 0` (COM-порт) не чіпайте, інакше перестануть працювати прошивка і монітор.

## Крок 2. Налаштування проєкту

### 2.1. Увімкнути SystemView у конфігурації

Варіант А, через menuconfig:

```
pio run -t menuconfig
```

Керування: стрілки - рух, Enter - зайти в меню, пробіл - увімкнути, Esc - назад, S - зберегти, Q - вийти.

У `Component config → Application Level Tracing`:

1. `Data Destination 1` → `JTAG`. Поки цього не зроблено, підменю SystemView не видно.
2. `FreeRTOS SystemView Tracing` → `[*] SystemView Tracing Enable`
3. Там же `Timer to use as timestamp source` → `esp_timer high resolution timer`

Решту не змінюйте: події задач, переривань і idle увімкнені за замовчуванням.

Варіант Б, через файл `sdkconfig.defaults` у корені проєкту:

```
CONFIG_APPTRACE_DEST_JTAG=y
CONFIG_APPTRACE_SV_ENABLE=y
CONFIG_APPTRACE_SV_TS_SOURCE_ESP_TIMER=y
```

Якщо проєкт уже збирався раніше, видаліть файл `sdkconfig.<назва середовища>` (наприклад, `sdkconfig.esp32-s3-devkitc-1`). Інакше PlatformIO не підхопить нові налаштування: `sdkconfig.defaults` він читає лише тоді, коли `sdkconfig` ще не існує.

Після зміни конфігурації збірка буде повна, це 2-3 хвилини.

### 2.2. Скрипт запису

1. Скопіюйте `sysview_target.py` з проєкту `6_2_SystemView` у корінь свого проєкту (поруч із `platformio.ini`).
2. Додайте в `platformio.ini` у своє середовище `[env:...]`:
   ```ini
   extra_scripts = sysview_target.py
   custom_sysview_seconds = 10
   custom_sysview_openocd = ~/.espressif/tools/openocd-esp32/v0.12.0-esp32-20251215/openocd-esp32
   ```

### 2.3. Код

Обов'язково змінювати нічого не треба. Задачі, перемикання між ними, переривання і блокування SystemView бачить сам.

Одна порада: давайте задачам зрозумілі імена (другий аргумент `xTaskCreate` / `xTaskCreatePinnedToCore`). SystemView показує саме їх, і на шкалі `"Sensor"` читається набагато легше, ніж `"task1"`.

### 2.4. Маркери (за бажанням)

Маркер виділяє на шкалі окрему ділянку коду, наприклад «читання датчика» всередині задачі:

```c
#if CONFIG_APPTRACE_SV_ENABLE
#include "SEGGER_SYSVIEW.h"
#define SV_MARK_START(id) SEGGER_SYSVIEW_MarkStart(id)
#define SV_MARK_STOP(id)  SEGGER_SYSVIEW_MarkStop(id)
#else
#define SV_MARK_START(id) ((void)(id))
#define SV_MARK_STOP(id)  ((void)(id))
#endif

#define MARK_SENSOR_READ 1U

/* у задачі */
SV_MARK_START(MARK_SENSOR_READ);
read_sensor();
SV_MARK_STOP(MARK_SENSOR_READ);
```

Обгортка `#if` потрібна, щоб код збирався і з вимкненим SystemView: тоді маркери просто зникають, і прибирати їх перед здачею не треба.

У `src/CMakeLists.txt` додайте `app_trace`:

```cmake
idf_component_register(SRCS ${app_sources}
                       PRIV_REQUIRES app_trace)
```

## Крок 3. Запис

```
pio run -t upload
pio run -t sysview
```

Або в панелі PlatformIO у VS Code: Project Tasks > (ваше середовище) > Custom > SystemView trace.

Скрипт скидає плату, записує 10 секунд роботи з обох ядер і зберігає `trace.SVDat` у корені проєкту. Запис починається зі старту прошивки, тому на шкалі видно, як `app_main` створює задачі.

Відкрийте файл у SystemView: File > Load Data > `trace.SVDat`.

Перед наступним записом закрийте файл у SystemView, інакше скрипт не зможе його перезаписати.

Монітор (`pio device monitor`) можна тримати відкритим паралельно.

## Крок 4. Як читати запис

Якщо якогось вікна не видно, увімкніть його через меню View.

### Яка задача на якому ядрі

- Задачі, створені через `xTaskCreatePinnedToCore`, працюють на ядрі з останнього аргументу (0 або 1).
- Задачі простою мають номер ядра в назві: `IDLE0`, `IDLE1`. Так само системні `ipc0`, `ipc1`.
- Задача `main` виконує `app_main` і завершується після його виходу (подія Task Terminate на початку запису).

### Contexts (таблиця задач і переривань)

Для кожної задачі: пріоритет, скільки разів активувалась, скільки часу виконувалась, відсоток CPU.

Activations - це скільки разів задача прокинулась, тобто перейшла з Blocked у Ready. Задача, яка засинає у `vTaskDelay` / `vTaskDelayUntil` або чекає на черзі, має багато активацій. Задача, яка ніколи не блокується, має одну активацію за весь запис: її створили, і вона так і лишається Ready. Коли таку задачу витісняє пріоритетніша, вона не засинає, а стоїть у Ready і чекає. Одна активація при великому відсотку CPU - ознака того, що задача ніколи не віддає процесор і може голодувати решту задач на своєму ядрі.

Якщо `IDLE0` або `IDLE1` має близько 0% CPU, задача простою на цьому ядрі не отримує часу, і через ~5 с task watchdog напише в лог `IDLE1 did not reset the watchdog`.

### Timeline (часова шкала)

Кожна задача і кожне переривання має свій рядок, смуга означає, що контекст зараз виконується. Колесом миші змінюйте масштаб.

Що шукати:
- Витіснення (preemption): довга смуга низькопріоритетної задачі, яку перериває коротка смуга пріоритетнішої.
- SysTick: тонкі риски кожні 10 мс на обох ядрах, це тік планувальника 100 Гц (`CONFIG_FREERTOS_HZ=100`). Звідси і точність `vTaskDelay`: менше одного тіку (10 мс) задача заснути не може.
- Маркери: ділянки між `SV_MARK_START` і `SV_MARK_STOP`. Їхню тривалість можна виміряти прямо на шкалі.

### Events List (список подій)

Усі події по порядку: Task Run, Task Stop, Task Ready, ISR Enter/Exit, Task Create, маркери. Якщо клацнути подію в списку, Timeline перейде на неї. Біля Task Stop видно, чим задача заблокувалась (`vTaskDelay`, `vTaskDelayUntil`, `xQueueReceive`...).

### CPU Load

Завантаження кожного ядра за часом.

### Terminal

Тут буде порожньо, і це нормально. `ESP_LOGI` пише в UART, а не в SystemView. У Terminal потрапляє тільки те, що виводиться через `SEGGER_SYSVIEW_Print`.

## Вправи

1. Знайдіть на Timeline момент, коли пріоритетніша задача витісняє менш пріоритетну. Що саме її будить? Подивіться, яке переривання стоїть прямо перед витісненням.
2. Виміряйте відстань між двома запусками задачі з `vTaskDelayUntil`. Замініть його на `vTaskDelay` з тим самим числом і порівняйте. `vTaskDelay` відраховує час від моменту виклику, а не від попереднього пробудження, тому період збільшується на час роботи задачі, округлений до тіків. Якщо робота коротша за тік (10 мс), різниці може не бути. Збільште роботу до 15 мс і порівняйте ще раз.
3. Знайдіть у своїй домашці задачу з однією активацією (або переконайтесь, що такої немає). Якщо є, додайте в неї блокування (`vTaskDelay`, чекання на черзі) і подивіться, як змінилися `IDLE` і Activations.

## Типові помилки

| Що бачимо | Причина | Що робити |
|---|---|---|
| `pio : The term 'pio' is not recognized` | `pio` не в PATH | Відкрити термінал PlatformIO (PlatformIO: New Terminal) або запустити `& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -t sysview` |
| `LIBUSB_ERROR_NOT_FOUND`, `could not find or open device` | Немає потрібного драйвера на Interface 2 | Крок 1.4 (Zadig), потім перепідключити кабель |
| `OpenOCD not found: ...` | OpenOCD не розпаковано або інший шлях | Крок 1.3 або виправити `custom_sysview_openocd` |
| SystemView: помилка 140 при відкритті | Запис зроблено старим OpenOCD 2022 року | Крок 1.3, записати заново |
| `SEGGER: Unsupported predef event 0` | Той самий старий OpenOCD | Крок 1.3 |
| `sysview: Invalid init seq`, файл 0 байт | Залишки попередньої сесії в буфері плати | Запустити запис ще раз; якщо повторюється - перепідключити кабель |
| `esp_usb_jtag: IN buffer overflow` | USB-JTAG у збійному стані після попередньої сесії | Запустити ще раз або перепідключити кабель |
| `Cannot overwrite ... close it in SystemView first` | Файл відкритий у SystemView | Закрити файл і повторити |
| У menuconfig немає варіанту JTAG | Увімкнено Power Management (`CONFIG_PM_ENABLE`) | Вимкнути PM. У проєктах з light sleep (урок 6.6) SystemView через JTAG не працює |
| Частина подій на шкалі без назв | Не замінено `SYSVIEW_FreeRTOS.txt` | Крок 1.2 |

## Що варто пам'ятати

- Трасування теж споживає процесорний час: кожна подія займає CPU і канал JTAG. Таймінги під трасуванням трохи відрізняються від звичайної роботи, тож для точних вимірів воно не еталон.
- Безкоштовна версія SystemView обмежує кількість подій у записі, тож записуйте коротко: 5-10 с.

## Джерела

- ESP-IDF: System Behavior Analysis with SEGGER SystemView - https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/tracing/sysview.html
- ESP-IDF v5.5: Application Level Tracing - https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-guides/app_trace.html
- Приклад ESP-IDF: `examples/system/sysview_tracing`
- SEGGER SystemView User Guide (UM08027), лежить у папці SystemView: `Doc\UM08027_SystemView.pdf`
