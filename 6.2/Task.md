# Урок 6.2 - FreeRTOS: задачі та планувальник

## Завдання

Створити систему з чотирьох задач із чітким розподілом обов'язків та пріоритетів.

### 1. Task "Sensor Reader" (пріоритет: Normal)

- Імітує опитування датчика. Виводить у UART: `[Sensor] Data read...`
- Строга періодичність - рівно кожні 500 мс. Звичайний `vTaskDelay` використовувати заборонено.

### 2. Task "Event Trigger" (пріоритет: Low)

- Імітує непередбачувану зовнішню подію.
- Засинає на 3 с через `vTaskDelay` / `osDelay`.
- Прокинувшись, динамічно створює нову задачу "Worker Task" і знову засинає.

### 3. Task "Worker Task" (пріоритет: High)

- Створюється лише при події (Task 2). Виводить `[Worker] Processing event...`
- Після цього обов'язково завершується через `vTaskDelete(NULL)` (або `osThreadExit()` / `osThreadTerminate()`). Вихід через `return` або `}` без видалення - заборонено!

### 4. Task "Maintenance Mode" (пріоритет: Realtime)

- Прокидається раз на 10 с.
- Через `vTaskSuspend(handle)` / `osThreadSuspend` ставить "Sensor Reader" на паузу.
- Виводить `[Maintenance] System paused for 2 seconds.`, чекає 2 с.
- Відновлює датчик через `vTaskResume` / `osThreadResume`.

## Матеріали

- Практика з уроку: [`6_2_Practice/`](6_2_Practice/)
- Еталон рішення: [`6_2_Reference/`](6_2_Reference/)
- Бонус для охочих - задачі під SEGGER SystemView: [`6_2_SystemView/`](6_2_SystemView/) (інструкція: `SystemView_для_студентів.md`)
