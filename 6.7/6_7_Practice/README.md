# Практика 6.7 - Heap, динамічне виділення памʼяті, оптимізація ресурсів

Код для живої демонстрації на уроці 6.7. Кроки 1-6 йдуть за розділом «Що показуємо покроково» з плану уроку, кроки 7-8 доповнено з теорії уроку (High Water Mark, Stack Overflow, Memory Pool), окремих кроків під них у плані немає. Плата ESP32-S3-DevKitC-1, PlatformIO, `framework = espidf`, чистий C17, процедурно.

| Крок | Середовище | Файл | Що показує | Час | Звідки |
|---|---|---|---|---|---|
| 1 | `step1` | `src/step1_monitor.c` | free heap, найбільший блок, мінімум, дамп регіонів, HWM стеку на старті | 7 хв | план |
| 2 | `step2` | `src/step2_static_vs_dynamic.c` | `xTaskCreate()` проти `xTaskCreateStatic()` при штучній нестачі heap | 8 хв | план |
| 3 | `step3` | `src/step3_fragmentation.c` | різнорозмірні `heap_caps_malloc()`/`heap_caps_free()`: розрив free і largest росте | 8 хв | план |
| 4 | `step4` | `src/step4_malloc_fail.c` | `malloc(25 КБ)` = NULL при 100+ КБ вільних, callback невдалого виділення | 8 хв | план |
| 5 | `step5` | `src/step5_alloc_once.c` | antipattern malloc/free у циклі задачі проти одного виділення на старті | 7 хв | план |
| 6 | `step6` | `src/step6_allocators.c` | `malloc()` / `pvPortMalloc()` / `heap_caps_malloc()` з INTERNAL / SPIRAM / DMA | 5 хв | план |
| 7 | `step7` | `src/step7_stack_hwm.c` | підбір стеку за HWM, переповнення стеку і свій `vApplicationStackOverflowHook()` | 7 хв | доповнено |
| 8 | `step8` | `src/step8_memory_pool.c` | Memory Pool: блоки фіксованого розміру, після старту heap не рухається | 7 хв | доповнено |

```bash
pio run -e step1 -t upload -t monitor
```

`pio run` без `-e` збирає лише `step1` (`default_envs`). Файл кроку вибирає `src/CMakeLists.txt`: середовище `stepN` збирає лише `src/stepN_*.c`. Для всіх кроків увімкнено `-Wextra -Wshadow -Wstrict-prototypes`.

Спільне:
- `include/practice.h` - `log_heap()` (лог), `plot_heap()` (рядок `free_kb:.. largest_kb:.. min_free_kb:..` для Serial Plotter / Teleplot), `stack_free_pct()`, `check_task_created()`, «баласт» heap (`heap_ballast_take()` / `heap_ballast_release()`, кроки 2 і 5);
- `include/fragment.h` - навмисна фрагментація heap (кроки 3 і 4).

Баласт і фрагментатор - інструменти демонстрації, не зразок для виробу.

Перемикачі на початку файлів (змінити, прошити знову):

| Файл | Перемикач | Значення |
|---|---|---|
| `step2_static_vs_dynamic.c` | `HUGE_STATIC_STACK` | 1 - статичний стек 400 КБ: помилка лінкера ще до прошивки |
| `step5_alloc_once.c` | `ALLOC_ONCE` | 0 - antipattern (malloc/free у циклі), 1 - одне виділення на старті |
| `step7_stack_hwm.c` | `STACK_OVERFLOW` | 1 - локальний масив більший за стек задачі |

sdkconfig. Кроки 1-5, 7, 8 - налаштування ESP-IDF за замовчуванням, без PSRAM. Кроку 6 потрібна PSRAM: кореневий `CMakeLists.txt` бачить файл `sdkconfig.step6.defaults` і передає його як `SDKCONFIG_DEFAULTS` лише для `step6` (за тією ж логікою «назва середовища», що й вибір файлу кроку). Змінив `sdkconfig.step6.defaults` - видали `sdkconfig.step6`, інакше PlatformIO візьме старий.

Чому кроки 1-5 без PSRAM. З `CONFIG_SPIRAM_USE_MALLOC` великі `malloc()` ідуть у 8 МБ PSRAM, і `esp_get_free_heap_size()` показує мегабайти: фрагментацію внутрішньої SRAM на такому фоні не видно. Тому PSRAM вмикаємо лише там, де вона - тема кроку.

## Розбіжності методички з ESP-IDF / ESP32-S3

| Методичка | Насправді (ESP-IDF 6.1, PlatformIO) | Що в коді |
|---|---|---|
| ESP-IDF v5.5.4 | PlatformIO збирає ESP-IDF 6.1.0 | API кроків є в обох версіях |
| `vApplicationMallocFailedHook()` спрацьовує при невдалому виділенні | В ESP-IDF не викликається: `configUSE_MALLOC_FAILED_HOOK` = 0, `pvPortMalloc()` - тонка обгортка над `heap_caps_malloc(size, MALLOC_CAP_INTERNAL \| MALLOC_CAP_8BIT)` (`components/freertos/heap_idf.c`) | `heap_caps_register_failed_alloc_callback()` (крок 4); опція `CONFIG_HEAP_ABORT_WHEN_ALLOCATION_FAILS` робить з невдачі `abort()` |
| «Розмір стеку в словах, `usStackDepth=2048` = 8192 байти» | В ESP-IDF `StackType_t = uint8_t`: глибина стеку в `xTaskCreate()` і результат `uxTaskGetStackHighWaterMark()` - у байтах. Слова - це ванільний FreeRTOS (STM32) | `static_assert(sizeof(StackType_t) == 1)` у кроці 2, усі стеки - `..._STACK_BYTES` |
| `configSUPPORT_STATIC_ALLOCATION=1` треба вмикати | В ESP-IDF статичне створення увімкнене завжди (`FreeRTOSConfig.h`), Kconfig-опцію лишено для сумісності | нічого не вмикаємо |
| «heap_5 - єдиний варіант для ESP32-S3» | ESP-IDF не використовує heap_1..heap_5 FreeRTOS: власний capabilities-алокатор (multi_heap на TLSF), кілька регіонів з різними `MALLOC_CAP_*` | проговорити як аналогію: «за змістом ближче до heap_5» |
| `MALLOC_CAP_DMA` і PSRAM | На S3 heap PSRAM не має прапора `MALLOC_CAP_DMA` (`heap/port/esp32s3/memory_layout.c`), DMA-виділення завжди у внутрішній SRAM | крок 6 друкує `esp_ptr_dma_capable()` |
| `MALLOC_CAP_EXEC` (IRAM, виконуваний код) | На S3 за замовчуванням увімкнений захист памʼяті (PMS), регіону з EXEC у heap немає. Більше того, без `CONFIG_HEAP_HAS_EXEC_HEAP` макрос `MALLOC_CAP_EXEC` навіть не визначено: код з ним не компілюється | крок 6 обгортає його в `#ifdef` і друкує 0 |
| `vApplicationStackOverflowHook()` | В ESP-IDF вже є власна реалізація, але оголошена `weak`: свою підставити можна | крок 7 |
| Плата N16R8 | `board = esp32-s3-devkitc-1` у PlatformIO описує 8 МБ flash без PSRAM. При збірці видно «Flash memory size mismatch ... 2MB»: це про налаштування flash у sdkconfig, на роботу кроків не впливає | PSRAM вмикає лише `sdkconfig.step6.defaults` (Octal, `CONFIG_SPIRAM_IGNORE_NOTFOUND`) |

## Крок 1 - моніторинг на старті

Знімок heap: усього, вільно, найбільший блок, мінімум, по регіонах (INTERNAL / DMA / SPIRAM), потім `heap_caps_print_heap_info()` - дамп кожного регіону. Далі `log_heap()` до і після `xTaskCreate(worker)`, раз на секунду рядок для плотера, раз на 5 с HWM задач `main` і `worker`.

```
I (320) monitor: esp_get_free_heap_size()                345000 B
I (320) monitor: heap_caps_get_largest_free_block()      237000 B
I (330) monitor: до xTaskCreate(worker)       free 345000 B | largest 237000 B | ...
I (330) monitor: після xTaskCreate(worker)    free 340600 B | largest 237000 B | ...
I (5340) monitor: HWM worker: 2100 B вільно з 4096 (51%)
```

(числа - ілюстрація)

Що проговорити:
- free і largest різні вже на старті: heap складається з кількох регіонів (DRAM, D/IRAM, RTC RAM), між регіонами блоки не зливаються. Дамп `heap_caps_print_heap_info()` це показує.
- Після `xTaskCreate()` free менший на стек + TCB: задача теж живе в heap. Повернемось у кроці 2.
- `worker` нічого не робить, крім `ESP_LOGI` раз на 2 с, а зʼїла близько 2 КБ стеку: printf-сімейство - головний споживач стеку.
- Лінії на плотері рівні: у циклі нічого не виділяється. Так і має виглядати здоровий пристрій. min_free_kb - «найгірший момент»: у виробі його логують / відправляють у телеметрію.
- Рефлексія з плану: STM32F4 має один регіон SRAM, ESP32-S3 - внутрішню SRAM з різними властивостями (DMA, IRAM) і PSRAM через SPI-шину. Тому на S3 алокатор приймає «побажання» `MALLOC_CAP_*`.

## Крок 2 - xTaskCreate() проти xTaskCreateStatic()

1) `xTaskCreate()` - free падає на ~4 КБ стеку + TCB. 2) `xTaskCreateStatic()` - free не змінюється. 3) Динамічну задачу видаляємо, «баласт» зʼїдає heap до 2 КБ. 4) Ще раз: `xTaskCreate()` повертає `errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY`, `xTaskCreateStatic()` створює задачу при порожньому heap.

```
I (330) static: після xTaskCreate          free 340200 B | ...
I (330) static: після xTaskCreateStatic    free 340200 B | ...
I (440) static: баласт зʼїв heap           free   2040 B | largest 1800 B | ...
E (440) static: xTaskCreate: помилка -1 (errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY = -1)
I (440) static: xTaskCreateStatic при порожньому heap: створено
```

Що проговорити:
- Памʼять статичної задачі зарезервував лінкер: вона у `.bss` (видно в рядку RAM при збірці: step2 займає на ~8 КБ більше за step1 - це два статичні стеки).
- `HUGE_STATIC_STACK 1` і просто `pio run -e step2`: збірка падає з «region `dram0_0_seg' overflowed». Це відповідь на питання рефлексії: нестача памʼяті під статичну задачу - помилка лінкування на столі розробника, а не відмова в runtime на обʼєкті.
- Результат `xTaskCreate()` перевіряємо завжди. Тут не `abort()`, щоб показати код помилки; у виробі - стратегія під критичність задачі.
- Видалена задача звільняє памʼять не одразу: це робить Idle Task. Тому після `vTaskDelete()` - пауза.
- Рекомендація з методички: критичні задачі (watchdog, звʼязок, safety) - статичні, решта - динамічні з перевіркою результату.

## Крок 3 - фрагментація

Заповнюємо heap блоками 8-20 КБ упритул, залишки в регіонах затикаємо «пробками» по 512 Б, потім звільняємо кожен другий блок, потім усе. Після кожного кроку рядок для плотера.

```
I (6400) frag: виділено 22 блоків і 31 пробок по 512 B
W (9500) frag: вільно 160 КБ, але найбільший шматок лише 20 КБ
```

Що проговорити:
- Це слайд «A=50, B=20, C=15 КБ, free(B)» на справжньому heap. Заповнення: free і largest падають разом. Звільнення: free росте, largest стоїть на ~20 КБ. Розрив між лініями і є фрагментація.
- Чому дірки не зливаються: сусіди кожної дірки зайняті. Злити алокатор може лише суміжні вільні блоки.
- min_free_kb впав до 0 і там лишиться: мінімум памʼятає найгірший момент назавжди.
- Прибирання: звільнили сусідів - largest повернувся. Фрагментацію лікує стратегія виділень, а не обсяг памʼяті.
- Аналогія: парковка. 20 вільних місць, але розкиданих по одному між машинами: автобус (25 КБ) не стане, хоча «місць багато».

## Крок 4 - malloc(25 КБ) = NULL

Та сама фрагментація, що в кроці 3, без графіка. `malloc(12 КБ)` проходить (є така дірка), `malloc(25 КБ)` - NULL при 100+ КБ вільних. Callback `heap_caps_register_failed_alloc_callback()` друкує, хто і скільки просив. Після прибирання той самий запит проходить.

```
I (6500) fail: malloc(25600): free 160000 B, largest 20480 B
!!! alloc failed: malloc(25600 B), caps 0x00001800
E (6500) fail: malloc(25600) = NULL - операцію пропущено
W (6500) fail: невдалих виділень (callback): 1
```

Що проговорити:
- Відповідь на hook-питання уроку: `esp_get_free_heap_size()` рахує суму шматків, а `malloc()` потрібен один суцільний.
- Callback - це діагностика, а не лікування: він викликається в контексті того, хто кликав `malloc`, там не можна виділяти памʼять і блокуватись. Тому всередині лише лічильник і `esp_rom_printf`.
- Перевірка NULL - обовʼязкова. Стратегія за критичністю: некритична задача - soft fail (як тут: лог і пропуск), важлива - safe mode, критична - `configASSERT()` і перезапуск.
- Чому не `vApplicationMallocFailedHook()`: див. таблицю розбіжностей.

## Крок 5 - одне виділення замість malloc/free у циклі

Задача обробляє кадри змінної довжини (256 Б - 12 КБ) і веде журнал останніх 32 подій (16-128 Б). Баласт лишає програмі 48 КБ heap, щоб ефект був видно за хвилину. Раз на секунду: `free_kb`, `largest_kb`, `alloc_fail`, а в режимі 0 ще розкид часу `malloc` у тактах CPU.

| `ALLOC_ONCE` | Що в циклі | Що на графіку |
|---|---|---|
| 0 | `malloc(len)` на кожен кадр і кожен запис журналу | largest скаче і просідає, зʼявляються alloc_fail, час malloc щоразу різний |
| 1 | буфер кадру під найгірший випадок - один `heap_caps_malloc()` при старті, журнал - статичний масив записів фіксованого розміру | рівні лінії, alloc_fail = 0 |

(конкретні числа залежать від розкладу heap після баласту, на уроці дивимось на форму ліній)

Що проговорити:
- Режим 0 компілюється без попереджень і навіть працює: погане не в синтаксисі, а в архітектурі. Суміш «різний розмір + різний час життя» (короткі кадри + довгоживучі записи) ріже heap.
- Недетермінований час: malloc шукає блок, і що більш порізаний heap, то довше. Для задачі з жорстким дедлайном це проблема навіть без NULL.
- Режим 1: памʼять під найгірший випадок узята один раз, поки heap суцільний (init-time allocation). Ціна - журнал займає 32 x 129 Б завжди, навіть порожній. Це свідомий обмін RAM на передбачуваність.
- Явний прапор `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` і перевірка NULL при старті: без буфера кадрів пристрій марний, тому падаємо одразу і голосно, а не через тиждень.

## Крок 6 - три алокатори і регіони

Таблиця виділень: `malloc` 1 КБ і 64 КБ, `pvPortMalloc` 64 КБ, `heap_caps_malloc` INTERNAL 64 КБ, DMA 4 КБ, SPIRAM 1 МБ, INTERNAL 1 МБ. Для кожного: адреса, SRAM чи PSRAM, чи годиться для DMA, скільки зайнято в heap насправді. Наприкінці - memset 64 КБ у SRAM проти PSRAM.

```
I (900) alloc: malloc(1 КБ)           -> 0x3fc9b2a0  SRAM   DMA: так  зайнято в heap 1028 B
I (900) alloc: malloc(64 КБ)          -> 0x3c0a0000  PSRAM  DMA: ні   ...
I (900) alloc: pvPortMalloc(64 КБ)    -> 0x3fca1000  SRAM   DMA: так  ...
W (910) alloc: caps INTERNAL 1 МБ     -> NULL (немає такого регіону або суцільного шматка)
I (920) alloc: memset 64 КБ у SRAM:  ... тактів
I (930) alloc: memset 64 КБ у PSRAM: ... тактів (у рази більше)
```

Що проговорити:
- В ESP-IDF усі три функції зрештою кличуть `heap_caps_malloc()`. Різниця - хто обирає регіон: libc (`malloc`: дрібне у SRAM, понад 16 КБ - може в PSRAM), порт FreeRTOS (`pvPortMalloc`: завжди внутрішня SRAM), розробник (`heap_caps_malloc` з явним прапором).
- Один і той самий `malloc(64 КБ)` з PSRAM і без неї потрапляє в різну памʼять. Для DMA-буфера SPI це різниця між «працює» і «не працює»: просимо `MALLOC_CAP_DMA` явно.
- EXEC = 0 і `#ifdef MALLOC_CAP_EXEC` у коді: на S3 захист памʼяті забороняє виконувати код з heap, і ESP-IDF прибирає сам прапор, щоб помилка була при збірці, а не в runtime.
- Швидкість: PSRAM за SPI-шиною і кешем. Великі буфери (кадр дисплея, аудіо) - туди, гарячі дані і стеки - у SRAM.
- Рефлексія з плану: на STM32F4 один регіон, вибирати нічого - `pvPortMalloc()` (heap_4, потокобезпечний). На S3 регіонів кілька - `heap_caps_malloc()` з прапором.
- Звільняти «парною» функцією. В ESP-IDF вони взаємозамінні, а на STM32 `free()` блоку з `pvPortMalloc()` (heap_4 + newlib) ламає heap.
- В ISR не виділяємо ніколи, навіть потокобезпечним алокатором: він бере mutex / критичну секцію і має недетермінований час. Ланцюжок: ISR -> черга -> задача -> виділення.

## Крок 7 - High Water Mark і переповнення стеку (доповнено)

Три задачі з однаковим стеком 3 КБ: `quiet` (лише лічильник), `logger` (`ESP_LOGI` з `%f`), `buffer` (локальний масив 512 Б). Раз на 2 с - використано, HWM у байтах і відсотках, вердикт за порогами 20% / 10% / 0.

```
I (2330) stack: quiet  використано  400 B, HWM 2672 B (86%) - ок
I (2330) stack: logger використано 2150 B, HWM  922 B (30%) - ок
I (2330) stack: buffer використано  950 B, HWM 2122 B (69%) - ок
```

`STACK_OVERFLOW 1`: масив дорівнює всьому стеку.

```
*** STACK OVERFLOW у задачі "buffer": збільшити стек або прибрати великі локальні масиви ***
abort() was called ... stack overflow
```

Що проговорити:
- Однаковий стек, різна робота - різний HWM. Стек підбирають вимірюванням: пік під найважчим сценарієм + 20-30% запасу.
- HWM - мінімум за весь час життя задачі. Гілку коду, яка не виконалась під час тесту (обробка помилки з логом!), він не бачить.
- HWM = 0 - вже не «запас на майбутнє», а факт: задача дійшла до межі, і canary, найімовірніше, вже затерто (питання рефлексії з плану).
- Canary перевіряється лише при перемиканні контексту: між переповненням і хуком памʼять сусідів уже могла зіпсуватись. Точніше ловить `CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK` (апаратна точка зупину на кінець стеку, ціною одного watchpoint).
- В ESP-IDF хук уже реалізований (друкує і робить abort), але `weak`: свій підставити можна. У хуку - мінімум роботи, стек уже зіпсований.

## Крок 8 - Memory Pool (доповнено)

Пул з 8 блоків по 256 Б у `.bss` і free-list через `union` (поки блок вільний, його перші байти - вказівник на наступний). Producer (статична задача) раз на 20 мс бере блок і шле вказівник через статичну чергу, consumer обробляє 15 мс і повертає блок. Раз на секунду пачка з 10 пакетів - пул вичерпується, частина відкидається.

```
I (330) pool: до ініціалізації      free 345000 B | ...
I (330) pool: після ініціалізації   free 345000 B | ...
pool_free:7 pool_min:0 dropped:3 heap_free_kb:336
```

Що проговорити:
- `pool_alloc()` / `pool_free()` - O(1), час завжди однаковий, фрагментації немає в принципі: будь-який вільний блок підходить під будь-який запит. Аналогія: гардероб з номерками.
- heap_free_kb - рівна лінія з першої секунди: пул, черга і обидві задачі статичні, після старту з heap не взято жодного байта. Free до і після ініціалізації однаковий.
- Через чергу йде вказівник (4 байти), а не пакет: власність блоку переходить до consumer, і повертає його той, хто закінчив роботу.
- Пул порожній - producer одразу відкидає пакет і рахує `dropped`, а не чекає безкінечно. Нестача видна в цифрах (backpressure), а не в загадкових зависаннях. pool_min підказує, чи вистачає 8 блоків.
- Два ядра: критична секція FreeRTOS на ESP32 - це spinlock (`portMUX_TYPE`) плюс заборона переривань, тому вона коротка: лише перестановка вказівників.
- Альтернатива free-list - статична черга вільних вказівників: та сама O(1), плюс можна чекати з таймаутом і брати блок з ISR через `...FromISR`.

## Навмисно не включено (це домашка)

Домашка 6.7 - рефакторинг коду «FRANKENSTEIN» (`../../6.7.HomeTask/src/6.7hometask.c`). Практика його не чіпає і готових відповідей на пункти чекліста з `../Task.md` не дає:
- `esp_timer`, АЦП, читання датчика в колбеку таймера і ланцюжок «колбек -> черга -> задача»;
- формування JSON / рядків, `sprintf` проти `snprintf`, великий масив на стеку задачі обробки;
- логіка повтору відправки і втрачені пакети при помилці Wi-Fi;
- обробка помилок ініціалізації в `app_main` і хендл таймера;
- переведення черги і задач FRANKENSTEIN на статичні варіанти та підбір їхнього стеку.

Кроки дають механізми на інших прикладах (статичні задачі і черги, HWM, одне виділення на старті, пул, перевірка NULL). Застосувати їх до FRANKENSTEIN - завдання студентів.

## 🚀 На виріст

- Heap tracing (`CONFIG_HEAP_TRACING_STANDALONE`, `heap_trace_start()` / `heap_trace_dump()`): пошук витоків з адресою і стеком викликів кожного невільненого блоку.
- `CONFIG_HEAP_POISONING_COMPREHENSIVE` + `heap_caps_check_integrity_all()`: ловить запис за межі блоку і використання після `free()` (той самий «CORRUPT HEAP» з методички, але ближче до місця помилки).
- `heap_caps_monitor_local_minimum_free_size_start()` / `_stop()`: мінімум heap за конкретний відрізок роботи, а не за весь час.
- `idf.py size` / `pio run -t size` і `size-components`: хто займає `.bss`, `.data`, IRAM. Тема наступного заняття.
- STM32 (Black Pill): `configUSE_MALLOC_FAILED_HOOK = 1` + `vApplicationMallocFailedHook()`, `xPortGetMinimumEverFreeHeapSize()` і `vPortGetHeapStats()` у heap_4 - там хук з методички працює як описано.

## Джерела

- [ESP-IDF: Heap Memory Allocation (ESP32-S3)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/mem_alloc.html)
- [ESP-IDF: Heap Memory Debugging](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/heap_debug.html)
- [ESP-IDF: FreeRTOS (IDF) - відмінності від ванільного FreeRTOS, стек у байтах](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/freertos_idf.html)
- [ESP-IDF: Support for External RAM (ESP32-S3)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/external-ram.html)
- [ESP-IDF: Fatal Errors - Stack overflow](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/fatal-errors.html)
- [FreeRTOS: Stack Usage and Stack Overflow Checking](https://www.freertos.org/Documentation/02-Kernel/02-Kernel-features/09-Memory-management/02-Stack-usage-and-stack-overflow-checking)
- [FreeRTOS: Static vs Dynamic Memory Allocation](https://www.freertos.org/Documentation/02-Kernel/02-Kernel-features/09-Memory-management/03-Static-vs-Dynamic-memory-allocation)
