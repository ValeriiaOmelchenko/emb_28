# 6.2 - задачі FreeRTOS під SEGGER SystemView (PlatformIO, ESP32-S3)

Бонус до уроку 6.2 для охочих. Консолі (`ESP_LOGI`, `vTaskList`) для домашки достатньо. SystemView показує те саме на таймлайні: яка задача на якому ядрі, коли спрацювало витіснення (preemption), хто голодує, скільки триває ISR.

## Що потрібно

- ESP32-S3-DevKitC-1, кабель у порт USB (не UART). Через нього працюють прошивка, монітор і JTAG, другий кабель не потрібен.
- SEGGER SystemView 3.60 або новіший (безкоштовний для некомерційного використання): https://www.segger.com/products/development-tools/systemview/
- J-Link не потрібен: дані йдуть через вбудований USB-JTAG ESP32-S3 і OpenOCD.

## Налаштування (один раз)

1. OpenOCD потрібної версії. Для ESP-IDF 5.5.3 потрібен `v0.12.0-esp32-20251215`: саме цю версію вказує ESP-IDF у `tools/tools.json`. OpenOCD, який ставить сам PlatformIO (`v0.11.0-esp32-20220706`, 2022 рік), не підходить. Він не розуміє пакети SystemView з ESP-IDF 5.5 і пише формат, який SystemView 4.x не відкриває (див. «Типові помилки»).
   - Завантажити архів: https://github.com/espressif/openocd-esp32/releases/download/v0.12.0-esp32-20251215/openocd-esp32-win64-0.12.0-esp32-20251215.zip
   - Розпакувати в PowerShell (шлях до архіву - свій):
     ```powershell
     Expand-Archive "$env:USERPROFILE\Downloads\openocd-esp32-win64-0.12.0-esp32-20251215.zip" "$env:USERPROFILE\.espressif\tools\openocd-esp32\v0.12.0-esp32-20251215"
     ```
   - Перевірити, що існує файл `%USERPROFILE%\.espressif\tools\openocd-esp32\v0.12.0-esp32-20251215\openocd-esp32\bin\openocd.exe`. Саме цей шлях записаний у `custom_sysview_openocd` у `platformio.ini`. Якщо розпакували в інше місце, виправте шлях там.
   - Якщо на комп'ютері вже встановлено ESP-IDF 5.5 через інсталятор Espressif, цей OpenOCD уже лежить у тій самій папці.
2. Драйвер USB-JTAG (лише Windows). Якщо OpenOCD пише `esp_usb_jtag: could not find or open device` або `LIBUSB_ERROR_NOT_FOUND`, постав драйвер WinUSB через Zadig (https://zadig.akeo.ie): Options > List All Devices, обрати `USB JTAG/serial debug unit (Interface 2)`, WinUSB, Replace Driver. Робити це треба навіть тоді, коли там уже написано WinUSB: Windows сама ставить стандартний WinUSB без запису `DeviceInterfaceGUID`, і OpenOCD через нього пристрій не відкриває. Interface 0 (COM-порт) не чіпати, інакше пропаде монітор і прошивка.
3. Опис подій ESP-IDF для SystemView. ESP-IDF нумерує події FreeRTOS по-своєму, тож без цього кроку частина подій на таймлайні буде з невідомими назвами. Скопіюй
   `%USERPROFILE%\.platformio\packages\framework-espidf\tools\esp_app_trace\SYSVIEW_FreeRTOS.txt`
   поверх `<папка SystemView>\Description\SYSVIEW_FreeRTOS.txt` (оригінал краще зберегти).

## Запуск

```
pio run -t upload        прошити
pio run -t sysview       записати trace.SVDat (10 с, обидва ядра)
```

Потім у SystemView: File > Load Data > `trace.SVDat`. Перед наступним записом файл треба закрити в SystemView, інакше скрипт не зможе його перезаписати.

Перед записом скрипт скидає плату, тому запис починається зі старту прошивки: на таймлайні видно, як `app_main` створює задачі.

У VS Code ціль `sysview` є в панелі PlatformIO: Project Tasks > esp32-s3-devkitc-1 > Custom > SystemView trace.

Якщо PowerShell пише `pio : The term 'pio' is not recognized`, відкрий термінал PlatformIO (PlatformIO: New Terminal), у ньому `pio` доступний. Або запускай за повним шляхом: `& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -t sysview`.

Тривалість запису задає `custom_sysview_seconds` у `platformio.ini`. Монітор (`pio device monitor`) можна тримати відкритим паралельно.

## Як увімкнути SystemView у своєму проєкті (menuconfig)

У цьому проєкті все вже налаштовано через `sdkconfig.defaults`. Щоб зробити те саме у своєму проєкті ESP-IDF, треба змінити в menuconfig лише 3 пункти.

### Запуск menuconfig

```
pio run -t menuconfig
```

Або в панелі PlatformIO: Platform > Run Menuconfig.

Керування: стрілки - рух, Enter - зайти в меню, пробіл - увімкнути/вимкнути, Esc - назад, S - зберегти, Q - вийти.

### Що вмикаємо

Усе знаходиться в `Component config → Application Level Tracing`:

1. `Data Destination 1` → `JTAG`
   Цей пункт сам вмикає `CONFIG_APPTRACE_ENABLE`. Поки його не виставлено, підменю SystemView не з'являється.
2. `FreeRTOS SystemView Tracing` → `[*] SystemView Tracing Enable`
   Після цього нижче з'являються решта налаштувань SystemView.
3. Там же `Timer to use as timestamp source` → `esp_timer high resolution timer`
   На двоядерному S3 за замовчуванням стоїть General Purpose Timer, він теж працює. `esp_timer` використано тому, що так зроблено в прикладі Espressif `sysview_tracing`.

### Що перевірити, але не змінювати

- `SystemView destination` → `Data destination JTAG`. Вже стоїть за замовчуванням.
- Усі події від `Trace Buffer Overflow Event` до `Timer Exit Event` (ISR, Task Start/Stop, Ready, Create, Idle) за замовчуванням увімкнені. Цей список - перелік того, що потрапить на таймлайн.
- `Data Destination 2` → `None`. Варіант через UART трасує лише одне ядро і потребує окремого USB-UART адаптера.
- Power Management (`CONFIG_PM_ENABLE`) має бути вимкнений, інакше варіант JTAG не з'явиться в списку. За замовчуванням він вимкнений.

Після S (Save) перезібрати проєкт. Збірка буде повна, це займе 2-3 хвилини.

### Що потрібно, крім menuconfig

- Код змінювати не обов'язково. Задачі, перемикання контексту й переривання SystemView бачить і без змін у коді.
- Маркери (`SEGGER_SYSVIEW_MarkStart/MarkStop`), як у `src/main.c`, додавати за бажанням. Для них потрібні `#include "SEGGER_SYSVIEW.h"` і `PRIV_REQUIRES app_trace` у `src/CMakeLists.txt`.
- Для запису потрібен OpenOCD (див. «Налаштування», крок 1). Найпростіше скопіювати у свій проєкт `sysview_target.py` і додати в `platformio.ini`:
  ```
  extra_scripts = sysview_target.py
  custom_sysview_seconds = 10
  custom_sysview_openocd = ~/.espressif/tools/openocd-esp32/v0.12.0-esp32-20251215/openocd-esp32
  ```
  Тоді запис робиться командою `pio run -t sysview`.

Драйвер WinUSB через Zadig і встановлення SystemView (розділ «Налаштування») варто зробити заздалегідь, до заняття: це найдовша частина, і на ній найчастіше застрягають.

## Що налаштовано і чому

| Файл | Що там |
|---|---|
| `sdkconfig.defaults` | `CONFIG_APPTRACE_DEST_JTAG`, `CONFIG_APPTRACE_SV_ENABLE`, мітки часу від `esp_timer`, увімкнені події задач/ISR/idle |
| `sysview_target.py` | Ціль `sysview`: запускає OpenOCD з `board/esp32s3-builtin.cfg`, через Tcl-порт 6666 скидає плату (`reset run`) і дає `esp sysview_mcore start/stop` |
| `platformio.ini` | `custom_sysview_openocd` - шлях до OpenOCD, `custom_sysview_seconds` - тривалість запису |
| `src/main.c` | Сценарій практики 6.2 з маркерами `SEGGER_SYSVIEW_MarkStart/MarkStop` |

`esp sysview_mcore` пише обидва ядра в один файл в офіційному багатоядерному форматі SystemView 3.60+ (у заголовку файлу `SEGGER SystemViewer V3.60` і рядки `Offset Core0/Core1`). OpenOCD спершу пише проміжні файли `trace.SVDat_core0/_core1`, скрипт видаляє їх після склеювання.

Якщо `sdkconfig.defaults` змінювали вже після першої збірки, видали `sdkconfig.esp32-s3-devkitc-1`: PlatformIO створює його з defaults лише один раз.

## Що дивитися на таймлайні

`HEAVY_YIELDS` у `src/main.c`:

- `0` (за замовчуванням). HeavyMath (пріоритет 2) ніколи не блокується. На ядрі 1 IDLE1 не з'являється взагалі, це голодування. Monitor (пріоритет 3) все одно витісняє HeavyMath кожні 100 мс точно за розкладом. Через ~5 с task watchdog напише в лог `IDLE1 did not reset the watchdog`, і на таймлайні видно чому.
- `1`. Після кожної порції HeavyMath віддає 1 тік (`vTaskDelay(1)`), і з'являється IDLE1.

Що ще видно:
- маркери 1/2/3 - «корисна робота» Heartbeat / HeavyMath / Monitor;
- `vTaskDelayUntil` у Monitor дає рівний період навіть тоді, коли ядро зайняте;
- переривання тіку (SysTick) і `ESP_LOGI` з Monitor, який займає UART.

Для порівняння з `vTaskDelay` заміни `vTaskDelayUntil` у Monitor на `vTaskDelay` і подивись, як «пливе» період. Робота Monitor (3 мс) коротша за тік (10 мс), тому з `vTaskDelay` період може лишитися 100 мс: `vTaskDelay` рахує від тіку, в якому його викликали. Щоб різницю було видно, збільш `MONITOR_WORK_US` до 15000.

## Типові помилки

| Що бачимо | Причина | Що робити |
|---|---|---|
| `pio : The term 'pio' is not recognized` | `pio` не в PATH | Термінал PlatformIO або повний шлях (див. «Запуск») |
| `LIBUSB_ERROR_NOT_FOUND`, `could not find or open device` | Немає драйвера WinUSB з `DeviceInterfaceGUID` на Interface 2 | Zadig (див. «Налаштування», крок 2), потім перепідключити кабель |
| `OpenOCD not found: ...` | OpenOCD не розпаковано або інший шлях | «Налаштування», крок 1, або виправити `custom_sysview_openocd` |
| SystemView при відкритті файлу: помилка 140 | Файл записаний старим OpenOCD 2022 року (заголовок `V2.42`) | Поставити OpenOCD `v0.12.0-esp32-20251215` і записати заново |
| `SEGGER: Unsupported predef event 0` | Той самий старий OpenOCD не розуміє пакети з ESP-IDF 5.5 | Те саме |
| `sysview: Invalid init seq`, файл 0 байт | У буфері трасування на платі залишки попередньої сесії | Скрипт скидає плату перед записом; якщо повторюється - перепідключити кабель |
| `esp_usb_jtag: IN buffer overflow` при старті | USB-JTAG у дивному стані після попередньої сесії | Запустити ще раз або перепідключити кабель |
| `Cannot overwrite ... close it in SystemView first` | Файл відкритий у SystemView | Закрити файл і запустити запис ще раз |
| `Flash memory size mismatch detected. Expected 8MB, found 2MB` | У `sdkconfig` за замовчуванням 2 MB, плата описана як N8 | На трасування не впливає |

## Обмеження, про які варто знати

- Трасування не безкоштовне: кожна подія займає CPU і JTAG. Таймінги під трасуванням трохи інші, ніж без нього, тож для точних вимірів воно не еталон.
- Light sleep / power management (`CONFIG_PM_ENABLE`) несумісні з SystemView через JTAG (Kconfig не дасть увімкнути).
- Безкоштовна версія SystemView обмежує кількість подій у записі, тому записи краще робити короткими (5-10 с).
- На STM32 SystemView працює через J-Link RTT. Клони ST-Link з комплекту BlackPill для цього не підходять.

## Джерела

- ESP-IDF: System Behavior Analysis with SEGGER SystemView - https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/tracing/sysview.html
- ESP-IDF v5.5: Application Level Tracing - https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-guides/app_trace.html
- Приклад ESP-IDF: `examples/system/sysview_tracing`
- OpenOCD для ESP32, релізи: https://github.com/espressif/openocd-esp32/releases
- SystemView release notes (`Doc/Release_SystemView.html` у папці SystemView): мультиядерна підтримка з версії 3.60
- SEGGER SystemView User Guide (UM08027)
