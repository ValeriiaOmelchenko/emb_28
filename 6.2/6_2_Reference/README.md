# Еталон 6.2 - FreeRTOS: задачі та планувальник

Чотири задачі з різними пріоритетами: датчик зі строгим періодом, генератор подій, одноразовий Worker і Maintenance, що ставить датчик на паузу. Плата ESP32-S3-DevKitC-1, PlatformIO, `framework = espidf`, чистий C17. Весь код - `src/main.c`.

Еталон враховує типові помилки з перевірених робіт групи (таблиця наприкінці).

## Як запустити

```bash
pio run -t upload -t monitor
```

Нічого підключати не треба, вивід - у UART (USB-порт `UART` на платі). `sdkconfig.defaults` ставить тік 1 мс (`CONFIG_FREERTOS_HZ=1000`), щоб часові мітки в лозі були точними до мілісекунди.

## Вивід

Кожен рядок починається з мітки часу в мс - по ній перевіряються періоди. Текст після мітки - точно як в умові. Очікуваний вивід (на платі ще не переганяли; абсолютні мітки залежать від часу старту):

```
=== 6.2: Sensor / Event / Worker / Maintenance, ядро 1 ===
   500 ms  [Sensor] Data read...
  1000 ms  [Sensor] Data read...
  ...
  3000 ms  [Event] Event occurred, creating Worker
  3000 ms  [Worker] Processing event...
  ...
  9500 ms  [Sensor] Data read...
 10000 ms  [Sensor] Data read...
 10001 ms  [Maintenance] System paused for 2 seconds.
 10001 ms  [Maintenance] stack free: sensor ..., event ..., maintenance ... B; heap free ... B
 12000 ms  [Event] Event occurred, creating Worker      <- датчик мовчить 2 с
 12000 ms  [Worker] Processing event...
 12500 ms  [Sensor] Data read...                         <- ОДИН рядок, на старій сітці 500 мс
 13000 ms  [Sensor] Data read...
  ...
 20000 ms  [Maintenance] System paused for 2 seconds.   <- рівно через 10 с, а не 12
```

Що перевірити в лозі:
- датчик: мітки кратні 500 мс, і після паузи немає пачки рядків з однаковим часом;
- Maintenance: 10000, 20000, 30000... (а не 10000, 22000, 34000);
- `[Worker]` іде одразу за `[Event]` з тією самою міткою - Worker важливіший і витісняє Event Trigger;
- `heap free` однаковий від паузи до паузи - Worker-и не течуть.

## Відповідність умові

| Вимога | Як зроблено |
|---|---|
| Sensor Reader, Normal, `[Sensor] Data read...` | `sensor_task`, `PRIO_NORMAL` |
| Строго кожні 500 мс, без `vTaskDelay` | `xTaskDelayUntil` + ресинхронізація після паузи |
| Event Trigger, Low, спить 3 с через `vTaskDelay` | `event_trigger_task`, `PRIO_LOW`, `vTaskDelay(EVENT_SLEEP)` |
| Прокинувшись, динамічно створює Worker | `xTaskCreatePinnedToCore(worker_task, ...)` з перевіркою `pdPASS` |
| Worker, High, `[Worker] Processing event...` | `worker_task`, `PRIO_HIGH` |
| Worker завершується через `vTaskDelete(NULL)` | так, без виходу через `}` |
| Maintenance, Realtime, раз на 10 с | `maintenance_task`, `PRIO_REALTIME`, `xTaskDelayUntil` |
| `vTaskSuspend` датчика, `[Maintenance] System paused for 2 seconds.`, 2 с, `vTaskResume` | так, суспенд лише в безпечній точці |

## Що в коді і чому так

### Усі задачі на одному ядрі

`xTaskCreatePinnedToCore(..., APP_CORE = 1)`. ESP32-S3 двоядерний: задачі без прив'язки розходяться по ядрах, і Realtime з Normal просто працюють паралельно - витіснення за пріоритетом, яке вивчаємо, не видно. На одному ядрі завжди виконується найважливіша готова задача: Worker перебиває Event Trigger, Maintenance - усіх.

### Строгий період датчика і пауза

`xTaskDelayUntil` рахує наступне пробудження від запланованого моменту, а не від «зараз», тому час друку не накопичується. Але після `vTaskSuspend` на 2 с датчик відстає на 4 періоди. `xTaskDelayUntil` з ціллю в минулому повертається одразу, і без захисту після resume вилітає пачка з 4-5 рядків за одну мілісекунду (так було в 6 з 7 робіт).

Аналогія: листоноша тиждень хворів, а потім приніс сім газет в один день. Для газет ще терпимо, а для датчика це 5 опитувань I2C підряд і 5 точок з однаковим часом.

Захист - після пробудження перевірити, наскільки запізнились:

```c
const TickType_t now = xTaskGetTickCount();
const TickType_t late = now - last_wake;          // беззнакова різниця: ок і при переповненні
if (late >= SENSOR_PERIOD) {                      // пропустили щонайменше один слот
  last_wake = now - (late % SENSOR_PERIOD);       // найближча точка старої сітки
  continue;                                       // і не читаємо поза розкладом
}
```

Чому не лише результат `xTaskDelayUntil` (`pdFALSE` = «затримки не було»): у момент `vTaskSuspend` датчик уже спав у `xTaskDelayUntil`, тож після resume цей виклик повертає `pdTRUE`, і перший «пізній» рядок пройшов би. Перевірка запізнення ловить обидва випадки. `continue` обовʼязковий: без нього лишається один рядок поза сіткою.

### Maintenance - теж `xTaskDelayUntil`

«Раз на 10 с» - така сама строга періодичність, як у датчика. `vTaskDelay(10000)` + `vTaskDelay(2000)` дають період 12 с (3 роботи з 7). Аналогія: будильник «через 10 хвилин після того, як прокинувся» проти будильника «о 7:00» - перший щодня зʼїжджає на час сніданку. Пауза 2 с всередині тіла - звичайний `vTaskDelay`: це «2 с від цього моменту», сітки 10 с вона не зсуває.

### `vTaskSuspend` тільки в безпечній точці

`printf` в ESP-IDF на час виводу тримає lock спільного `stdout`. Якщо призупинити датчик посеред `printf`, наступний `printf` у самій Maintenance чекатиме на lock вічно, а `vTaskResume` стоїть уже після нього. Аналогія: прибиральник зачинив кухаря в коморі разом з єдиним ключем від кухні, а потім сам пішов на кухню.

До того ж 10 000 мс кратні 500 мс: кожні 10 с датчик і Maintenance прокидаються на одному тіку. Тому `wait_sensor_idle()` чекає, поки `eTaskGetState(s_sensor) == eBlocked` (датчик спить у `xTaskDelayUntil` і нічого не тримає), і лише тоді суспендить. На одному ядрі Maintenance найважливіша, тож між перевіркою і `vTaskSuspend` датчик виконатись не встигне. Очікування обмежене 100 мс: якщо датчик так і не заснув, обслуговування пропускається з рядком у лозі, а не висить. У лозі це видно так: о 10000 мс датчик ще дописує свій рядок, а пауза починається з 10001 мс.

Повне рішення для спільного ресурсу - мʼютекс (наступні уроки модуля): друк під мʼютексом, і Maintenance бере той самий мʼютекс перед `vTaskSuspend`.

### Перевірка створення задач

Кожен `xTaskCreatePinnedToCore` перевіряється через `!= pdPASS`:
- старт: задача не створилась - `abort()` з назвою. Датчик створюється першим: якби його хендл лишився `NULL`, `vTaskSuspend(NULL)` призупинив би саму Maintenance, і система мовчки завмерла б;
- Worker: не вистачило heap - рядок `ERROR` з вільним heap, система працює далі, але подія не губиться мовчки.

Пастка з робіт: `ok &= xTaskCreate(...)` не ловить помилку, бо `pdPASS` = 1, помилка = -1 (усі біти одиниці), і `1 & -1 == 1`.

### Стек і heap - виміряні, а не «на око»

Під час паузи Maintenance друкує `uxTaskGetStackHighWaterMark` (в ESP-IDF - у байтах) для датчика, Event Trigger і себе, а також `esp_get_free_heap_size()`. 3072 байти стеку взято з запасом під `printf` (зʼїдає 1,5-2 КБ); реальний запас видно в лозі. Heap має бути стабільним: стек і TCB видаленого Worker звільняє Idle-задача ядра 1.

### Константи і пріоритети

Періоди - `#define` через `pdMS_TO_TICKS`, а не сирі тіки (не ламаються при зміні частоти тіку). Пріоритети - відносно `tskIDLE_PRIORITY`, порядок і те, що Realtime нижчий за системний `esp_timer`, перевіряє `static_assert` на етапі збірки.

## Помилки з перевірених робіт 6.2 і як вони закриті тут

| Помилка | Скільки робіт | Тут |
|---|---|---|
| Пачка рядків датчика після resume | 6 з 7 | перевірка запізнення + ресинхронізація на сітку + `continue` |
| Maintenance раз на 12 с (`vTaskDelay` + `vTaskDelay`) | 3 | `xTaskDelayUntil` з періодом 10 с |
| `vTaskSuspend` посеред `printf` | ризик у 6 з 7 | одне ядро + суспенд лише в стані `eBlocked` |
| Задачі без прив'язки до ядра на S3 | 3 | усі на ядрі 1 |
| `xTaskCreate` не перевіряється / `ok &= ...` | 4 | `!= pdPASS` для кожної задачі |
| Стек «на око», 2048 з `printf` | 7 | 3072 + друк `uxTaskGetStackHighWaterMark` |
| Текст не як в умові | 2 | рядки з умови дослівно, мітка часу лише перед ними |
| Неініціалізовані буфери в лозі | 1 | рядки - літерали у `printf`, буферів немає |

## Що здати разом із кодом

- Лог щонайменше за 25 с (дві паузи Maintenance), з мітками часу.
- По логу: період датчика, період Maintenance, що робить датчик після resume.
- Виміряний запас стеку задач.

## 🚀 На виріст (поза програмою уроку)

- Мʼютекс для друку - повноцінний захист `stdout`, який не залежить від того, на якому ядрі задачі.
- Замість створення Worker на кожну подію - одна постійна задача, яку будить task notification або черга (6.3): без алокації heap під час роботи.
- `xTaskCreateStatic()` - задачі в статичних буферах, без heap взагалі.
- SystemView (`../6_2_SystemView/`) - видно, на якому ядрі і коли працює кожна задача.

## Джерела

- FreeRTOS: `xTaskDelayUntil` - https://www.freertos.org/Documentation/02-Kernel/04-API-references/02-Task-control/02-xTaskDelayUntil
- FreeRTOS: `vTaskSuspend` (з `NULL` призупиняє викликаючу задачу) - https://www.freertos.org/Documentation/02-Kernel/04-API-references/02-Task-control/06-vTaskSuspend
- FreeRTOS: `eTaskGetState` - https://www.freertos.org/Documentation/02-Kernel/04-API-references/03-Task-utilities/00-Task-utilities
- FreeRTOS: `xTaskCreate` (коди повернення) - https://www.freertos.org/Documentation/02-Kernel/04-API-references/01-Task-creation/01-xTaskCreate
- ESP-IDF (ESP32-S3): FreeRTOS SMP, прив'язка до ядер, стек у байтах - https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/freertos_idf.html
- ESP-IDF: пріоритети системних задач - https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/performance/speed.html
