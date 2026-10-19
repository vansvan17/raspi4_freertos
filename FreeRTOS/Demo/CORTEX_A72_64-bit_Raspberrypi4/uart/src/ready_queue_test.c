#include <stdint.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "board.h"
#include "uart.h"

#define PEER_PRIORITY 3U
#define PREEMPTOR_PRIORITY 5U

volatile unsigned int benchmark_context_switches;
volatile uint32_t secondary_core_release;

static QueueHandle_t release_peer;
static QueueHandle_t release_preemptor;
static volatile uint32_t trace_index;
static volatile uint32_t trace_values[5];

void secondary_core_main(uint64_t core_id)
{
    (void)core_id;
    for (;;) __asm volatile ("wfe");
}

static void gic_init(void)
{
    *(volatile uint32_t *)(GICD_BASE + 0x000U) = 0U;
    *(volatile uint32_t *)(GICC_BASE + 0x000U) = 0U;
    *(volatile uint32_t *)(GICC_BASE + 0x004U) = 0xFFU;
    *(volatile uint32_t *)(GICC_BASE + 0x008U) = 0U;
    *(volatile uint32_t *)(GICD_BASE + 0x000U) = 1U;
    *(volatile uint32_t *)(GICC_BASE + 0x000U) = 1U;
    __asm volatile ("dsb sy\n\tisb sy");
}

static void record(uint32_t value)
{
    uint32_t index = trace_index;
    if (index < 5U)
    {
        trace_values[index] = value;
        trace_index = index + 1U;
    }
}

static void peer_task(void *argument)
{
    uint32_t token;
    (void)argument;
    for (;;)
    {
        xQueueReceive(release_peer, &token, portMAX_DELAY);
        record(4U);
    }
}

static void preemptor_task(void *argument)
{
    uint32_t token;
    (void)argument;
    for (;;)
    {
        xQueueReceive(release_preemptor, &token, portMAX_DELAY);
        record(3U);
    }
}

static void running_task(void *argument)
{
    const uint32_t token = 1U;
    uint32_t index;
    (void)argument;

    record(1U);
    xQueueSend(release_peer, &token, 0U);
    record(2U);
    xQueueSend(release_preemptor, &token, 0U);
    record(5U);

    uart_puts("ready_queue_after_preemption=");
    if ((trace_values[2] == 3U) && (trace_values[3] == 4U))
        uart_puts("READY_PEER_FIRST");
    else if ((trace_values[2] == 3U) && (trace_values[3] == 5U))
        uart_puts("PREEMPTED_TASK_FIRST");
    else
        uart_puts("UNEXPECTED");
    uart_puts(" trace=");
    for (index = 0U; index < 5U; index++) uart_puthex(trace_values[index]);
    uart_puts("\r\n");

    for (;;) vTaskDelay(pdMS_TO_TICKS(1000U));
}

void main(void)
{
    gic_init();
    uart_init();
    release_peer = xQueueCreate(1U, sizeof(uint32_t));
    release_preemptor = xQueueCreate(1U, sizeof(uint32_t));

    uart_puts("\r\nFreeRTOS ready-queue placement experiment\r\n");
    xTaskCreate(preemptor_task, "preemptor", 512U, 0, PREEMPTOR_PRIORITY, 0);
    xTaskCreate(peer_task, "peer", 512U, 0, PEER_PRIORITY, 0);
    xTaskCreate(running_task, "running", 512U, 0, PEER_PRIORITY, 0);
    vTaskStartScheduler();
    for (;;) __asm volatile ("wfi");
}

void vApplicationIdleHook(void) {}
void vApplicationTickHook(void) {}
