// Крок 8 (доповнено, в плані уроку - лише в теорії): Memory Pool як промислова заміна malloc у циклі.
// Нічого підключати не треба. Serial Plotter: pool_free, pool_min, dropped, heap_free_kb.
//
// Пул - масив блоків ОДНАКОВОГО розміру у .bss і список вільних блоків (free-list).
//   pool_alloc() - зняти перший блок зі списку: O(1), час завжди однаковий;
//   pool_free()  - повернути блок на початок списку: O(1);
//   фрагментації немає в принципі: будь-який вільний блок підходить під будь-який запит.
// Аналогія: гардероб з номерками. Номерків рівно 8, усі гачки однакові. Скінчились номерки -
// гардеробник одразу каже «немає місць», а не шукає по залу, куди б ще втиснути пальто.
//
// Producer (static task) раз на 20 мс бере блок, заповнює пакет і шле ВКАЗІВНИК через чергу
// (xQueueCreateStatic) - копіюється 4 байти, а не весь пакет. Consumer обробляє 15 мс і
// повертає блок у пул. Раз на секунду producer видає пачку з 10 пакетів: пул (8 блоків)
// вичерпується, частина пакетів відкидається - це видно в dropped, а heap при цьому НЕ рухається:
// після старту програма не виділяє з heap жодного байта (heap_free_kb - рівна лінія).

#include <assert.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "practice.h"

#define POOL_BLOCKS        UINT32_C(8)
#define POOL_BLOCK_B       UINT32_C(256)
#define PAYLOAD_B          UINT32_C(200)
#define QUEUE_LEN          POOL_BLOCKS     // у черзі не може бути більше пакетів, ніж блоків
#define PRODUCE_PERIOD_MS  UINT32_C(20)
#define BURST_EVERY        UINT32_C(50)    // кожен 50-й період (раз на секунду) - пачка
#define BURST_PACKETS      UINT32_C(10)    // більше, ніж блоків у пулі
#define CONSUME_MS         UINT32_C(15)
#define REPORT_PERIOD_MS   UINT32_C(1000)
#define TASK_STACK_BYTES   UINT32_C(3072)  // ESP-IDF: у байтах
#define PRIO_PRODUCER      ((UBaseType_t)3)
#define PRIO_CONSUMER      ((UBaseType_t)2)

static const char *TAG = "pool";

// ---------------- Пул блоків фіксованого розміру ----------------
// union: поки блок вільний, його перші байти - вказівник на наступний вільний.
// Окремої памʼяті під список не треба.
typedef union pool_block {
  union pool_block *next;
  uint8_t data[POOL_BLOCK_B];
} pool_block_t;

static pool_block_t s_pool[POOL_BLOCKS];
static pool_block_t *s_free_head;
static uint32_t s_free_count;
static uint32_t s_free_min;
// Дві задачі на двох ядрах: критична секція FreeRTOS на ESP32 = spinlock + заборона переривань.
static portMUX_TYPE s_pool_lock = portMUX_INITIALIZER_UNLOCKED;

static void pool_init(void) {
  for (uint32_t i = 0; i < POOL_BLOCKS - 1U; i++) {
    s_pool[i].next = &s_pool[i + 1U];
  }
  s_pool[POOL_BLOCKS - 1U].next = NULL;
  s_free_head = &s_pool[0];
  s_free_count = POOL_BLOCKS;
  s_free_min = POOL_BLOCKS;
}

// NULL - пул порожній. Не блокується: що робити з нестачею, вирішує той, хто кличе.
static void *pool_alloc(void) {
  taskENTER_CRITICAL(&s_pool_lock);
  pool_block_t *b = s_free_head;
  if (b != NULL) {
    s_free_head = b->next;
    s_free_count--;
    if (s_free_count < s_free_min) {
      s_free_min = s_free_count;
    }
  }
  taskEXIT_CRITICAL(&s_pool_lock);
  return b;
}

static void pool_free(void *p) {
  pool_block_t *b = p;
  // Захист від чужих вказівників: повернути можна лише блок з нашого масиву.
  configASSERT((b >= &s_pool[0]) && (b < &s_pool[POOL_BLOCKS]));
  taskENTER_CRITICAL(&s_pool_lock);
  b->next = s_free_head;
  s_free_head = b;
  s_free_count++;
  taskEXIT_CRITICAL(&s_pool_lock);
}

static void pool_stats(uint32_t *free_now, uint32_t *free_min) {
  taskENTER_CRITICAL(&s_pool_lock);
  *free_now = s_free_count;
  *free_min = s_free_min;
  taskEXIT_CRITICAL(&s_pool_lock);
}

// ---------------- Пакет, що лежить у блоці пулу ----------------
typedef struct {
  uint32_t seq;
  uint16_t len;
  uint8_t payload[PAYLOAD_B];
} packet_t;

static_assert(sizeof(packet_t) <= POOL_BLOCK_B, "пакет має влазити в блок пулу");

// ---------------- RTOS-обʼєкти: усе статичне, heap не потрібен ----------------
static StaticQueue_t s_queue_buf;
static uint8_t s_queue_storage[QUEUE_LEN * sizeof(packet_t *)];
static QueueHandle_t s_queue;

static StackType_t s_producer_stack[TASK_STACK_BYTES];
static StaticTask_t s_producer_tcb;
static StackType_t s_consumer_stack[TASK_STACK_BYTES];
static StaticTask_t s_consumer_tcb;

static atomic_uint s_dropped; // пише producer, читає app_main

// Створити один пакет. false - відкинули (немає блоку або місця в черзі).
static bool produce_one(uint32_t seq) {
  packet_t *pkt = pool_alloc();
  if (pkt == NULL) {
    return false; // пул порожній: споживач не встигає. Відкидаємо, а не чекаємо без кінця.
  }
  pkt->seq = seq;
  pkt->len = (uint16_t)PAYLOAD_B;
  memset(pkt->payload, (int)(seq & UINT32_C(0xFF)), PAYLOAD_B);
  if (xQueueSend(s_queue, &pkt, 0) != pdTRUE) {
    pool_free(pkt); // блок не пішов далі - повертаємо, інакше пул «витече»
    return false;
  }
  return true;
}

static void producer_task(void *arg) {
  (void)arg;
  TickType_t last_wake = xTaskGetTickCount();
  uint32_t seq = 0;
  for (uint32_t period = 1;; period++) {
    const uint32_t count = ((period % BURST_EVERY) == 0) ? BURST_PACKETS : UINT32_C(1);
    for (uint32_t i = 0; i < count; i++) {
      if (!produce_one(++seq)) {
        atomic_fetch_add(&s_dropped, 1U);
      }
    }
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(PRODUCE_PERIOD_MS));
  }
}

static void consumer_task(void *arg) {
  (void)arg;
  uint32_t checksum = 0;
  for (;;) {
    packet_t *pkt = NULL;
    if (xQueueReceive(s_queue, &pkt, portMAX_DELAY) == pdTRUE) {
      for (uint32_t i = 0; i < pkt->len; i++) {
        checksum += pkt->payload[i];
      }
      vTaskDelay(pdMS_TO_TICKS(CONSUME_MS)); // «повільна» обробка: передача, запис у флеш тощо
      pool_free(pkt);                        // блок повертає той, хто закінчив з ним працювати
    }
  }
}

void app_main(void) {
  log_heap(TAG, "до ініціалізації");

  pool_init();
  s_queue = xQueueCreateStatic(QUEUE_LEN, sizeof(packet_t *), s_queue_storage, &s_queue_buf);
  configASSERT(s_queue != NULL);
  // Виклик - окремим рядком, не всередині configASSERT(): якщо асерти вимкнуть, зникне й виклик.
  const TaskHandle_t producer = xTaskCreateStatic(producer_task, "producer", TASK_STACK_BYTES, NULL, PRIO_PRODUCER,
                                                  s_producer_stack, &s_producer_tcb);
  const TaskHandle_t consumer = xTaskCreateStatic(consumer_task, "consumer", TASK_STACK_BYTES, NULL, PRIO_CONSUMER,
                                                  s_consumer_stack, &s_consumer_tcb);
  configASSERT((producer != NULL) && (consumer != NULL));

  log_heap(TAG, "після ініціалізації"); // ті самі числа: пул, черга і задачі - у .bss
  ESP_LOGI(TAG, "пул: %" PRIu32 " блоків x %" PRIu32 " B = %" PRIu32 " B у .bss", POOL_BLOCKS, POOL_BLOCK_B,
           (uint32_t)sizeof(s_pool));

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(REPORT_PERIOD_MS));
    uint32_t free_now = 0;
    uint32_t free_min = 0;
    pool_stats(&free_now, &free_min);
    printf("pool_free:%" PRIu32 " pool_min:%" PRIu32 " dropped:%u heap_free_kb:%" PRIu32 "\n", free_now, free_min,
           atomic_load(&s_dropped), esp_get_free_heap_size() / BYTES_PER_KB);
  }
}
