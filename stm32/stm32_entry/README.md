# stm32_entry - перший проєкт на STM32

Вступ до STM32: створити проєкт у STM32CubeIDE, прошити плату і побачити вивід в UART. Програма раз на секунду надсилає в USART1 рядок `Hello, STM32!`.

- Плата: STM32F411CEU6 BlackPill (WeAct).
- Середовище: STM32CubeIDE, HAL, FreeRTOS (CMSIS-RTOS v2).
- Тактування: внутрішній генератор HSI 16 МГц, без PLL.
- UART: USART1, 115200 бод, 8N1, PA9 - TX, PA10 - RX.

## Що потрібно

- BlackPill STM32F411CEU6;
- програматор ST-LINK V2 (для прошивки через SWD);
- USB-UART адаптер на 3,3 В (CP2102, CH340, FT232 тощо);
- дроти «мама-мама».

UART-адаптер має працювати з логікою 3,3 В. Якщо на адаптері є перемикач 5 В / 3,3 В, ставимо 3,3 В. PA9 і PA10 витримують 5 В (позначка FT у datasheet), але інші піни STM32 - ні, тому привчаємося одразу.

## Підключення

### ST-LINK V2 → BlackPill (прошивка, SWD)

На торці BlackPill є 4 піни для програматора. Порядок дивимось за підписами на самій платі.

| ST-LINK V2 | BlackPill |
|---|---|
| 3.3V | 3V3 |
| SWDIO | DIO (PA13) |
| SWCLK | CLK / SCK (PA14) |
| GND | GND |

Якщо плата вже живиться від USB-C, лінію 3.3V від ST-LINK не підключаємо. Два джерела живлення одночасно - погана ідея. Вистачає SWDIO, SWCLK і GND.

### USB-UART адаптер → BlackPill (вивід)

| Адаптер | BlackPill |
|---|---|
| RX | PA9 (USART1 TX) |
| TX | PA10 (USART1 RX) |
| GND | GND |

- TX іде на RX, хрест-навхрест: один пристрій говорить, інший слухає.
- GND спільний обовʼязково. Без нього рівні сигналів не мають спільної «нульової» точки, і в терміналі буде сміття або нічого.
- VCC адаптера до плати не підключаємо, плата живиться від ST-LINK або USB-C.

```
 ST-LINK V2            BlackPill F411             USB-UART
 ----------            --------------             --------
 SWDIO  ------------>  PA13 (DIO)
 SWCLK  ------------>  PA14 (CLK)
 GND    ------------>  GND  <---------------------  GND
 3.3V   ------------>  3V3
                       PA9  (TX)  --------------->  RX
                       PA10 (RX)  <---------------  TX
```

## Відкрити проєкт

1. Запустити STM32CubeIDE. Як workspace вибрати папку `stm32/`.
2. Якщо проєкту немає в Project Explorer: File → Open Projects from File System → вказати `stm32/stm32_entry/STM32CubeIDE`.
3. Конфігурація пінів і периферії - у `stm32_entry.ioc` (подвійний клік відкриває CubeMX). Після змін - Project → Generate Code.

Свій код пишемо лише між коментарями `/* USER CODE BEGIN ... */` і `/* USER CODE END ... */`. Усе поза ними CubeMX перезапише під час наступної генерації.

## Прошити через ST-LINK (основний спосіб)

1. Підключити ST-LINK до плати, а потім до USB компʼютера.
2. Зібрати: Project → Build Project (`Ctrl+B`). У Console має бути `Build Finished. 0 errors`.
3. Прошити і запустити: Run → Run As → STM32 C/C++ Application.
4. Під час першого запуску відкриється вікно Edit Configuration. На вкладці Debugger перевірити: Debug probe - ST-LINK (ST-LINK GDB server), Interface - SWD. Натиснути OK.
5. У Console має зʼявитися `Download verified successfully`, після чого програма стартує.

Для покрокового дебагу: Run → Debug As → STM32 C/C++ Application (`F11`). Програма зупиниться на початку `main()`, далі `F8` - продовжити, `F6` - крок.

Якщо ST-LINK не бачить плату:
- перевірити, що GND справді спільний і SWDIO/SWCLK не переплутані;
- оновити прошивку ST-LINK: Help → ST-LINK Upgrade;
- у Debug Configuration на вкладці Debugger поставити Reset behaviour = Connect under reset і під час підключення затиснути NRST на платі.

## Прошити через USB (DFU, без програматора)

У STM32F411 є заводський завантажувач у системній памʼяті, який уміє прошиватися по USB.

1. Поставити STM32CubeProgrammer (окрема програма від ST).
2. Зібрати проєкт у CubeIDE. Файл прошивки - `STM32CubeIDE/Debug/stm32_entry.elf`.
3. Від'єднати від PA10 USB-UART адаптер. Завантажувач слухає і USB, і USART1: шум на PA10 може переключити його в режим UART, і USB-пристрій не зʼявиться.
4. Підключити плату по USB-C, затиснути BOOT0, коротко натиснути NRST, відпустити BOOT0. Плата перейде в режим DFU.
5. У STM32CubeProgrammer вибрати USB → Refresh → Connect, відкрити `.elf` і натиснути Download.
6. Натиснути NRST, щоб запустити програму.

Дебагу при цьому немає: DFU лише записує прошивку. Для роботи на уроці зручніший ST-LINK.

## Подивитись вивід

Відкрити термінал на COM-порту USB-UART адаптера, 115200 бод. Номер порту в Windows - у Диспетчері пристроїв, розділ «Порти (COM і LPT)».

Будь-який із варіантів:
- PlatformIO (вже встановлений): `pio device monitor -b 115200`, а якщо портів кілька - `pio device monitor -b 115200 -p COM5`;
- STM32CubeIDE: Window → Show View → Console, потім кнопка Open Console → Command Shell Console → Serial Port;
- PuTTY: Connection type = Serial, Speed = 115200.

Має йти рядок раз на секунду:

```
Hello, STM32!
Hello, STM32!
```

Якщо нічого немає або йде сміття:
- TX і RX переплутані - поміняти місцями;
- немає спільного GND;
- швидкість у терміналі не 115200;
- відкрито не той COM-порт: ST-LINK теж може створювати віртуальний COM-порт.

## Як влаштований код

Весь код - у `Src/main.c`, у задачі `StartDefaultTask`.

- Проєкт з FreeRTOS: після `osKernelStart()` керування в `main()` не повертається. Тому `while (1)` в `main()` ніколи не виконується, і друк живе в задачі.
- `HAL_UART_Transmit()` - блокуюча передача з таймаутом. Повертає `HAL_OK` або помилку. Помилки рахуються в `uart_tx_errors`, лічильник видно в дебагері: Window → Show View → Live Expressions.
- `osDelayUntil()`, а не `osDelay(1000)`: період рахується від запланованого моменту, тому час самої передачі не додається до кожної секунди.
- Рядок - `static const char[]`: лежить у флеші, а не в RAM.

## Джерела

- STM32F411xC/xE datasheet (піни, FT-піни) - https://www.st.com/resource/en/datasheet/stm32f411ce.pdf
- RM0383, Reference manual STM32F411 (USART) - https://www.st.com/resource/en/reference_manual/rm0383-stm32f411xce-advanced-armbased-32bit-mcus-stmicroelectronics.pdf
- AN2606, STM32 system memory boot mode (DFU, вибір інтерфейсу завантажувача) - https://www.st.com/resource/en/application_note/an2606-stm32-microcontroller-system-memory-boot-mode-stmicroelectronics.pdf
- UM2609, STM32CubeIDE user guide - https://www.st.com/resource/en/user_manual/um2609-stm32cubeide-user-guide-stmicroelectronics.pdf
- WeAct BlackPill STM32F411 (схема і розпіновка) - https://github.com/WeActStudio/WeActStudio.MiniSTM32F4x1
