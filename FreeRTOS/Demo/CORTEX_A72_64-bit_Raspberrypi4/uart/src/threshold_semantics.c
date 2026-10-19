#include <stdint.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "board.h"
#include "uart.h"

#define TASK_A_PRIORITY 3U
#define TASK_B_PRIORITY 4U
#define TASK_C_PRIORITY 5U

volatile unsigned int benchmark_context_switches;
volatile uint32_t secondary_core_release;

static QueueHandle_t release_b;
static QueueHandle_t release_c;
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

static void task_b(void *argument)
{
    uint32_t token;
    (void)argument;
    for (;;)
    {
        xQueueReceive(release_b, &token, portMAX_DELAY);
        record(4U);
    }
}

static void task_c(void *argument)
{
    uint32_t token;
    (void)argument;
    for (;;)
    {
        xQueueReceive(release_c, &token, portMAX_DELAY);
        record(3U);
    }
}

static void task_a(void *argument)
{
    const uint32_t token = 1U;
    const uint32_t expected[5] = { 1U, 2U, 3U, 4U, 5U };
    uint32_t index;
    BaseType_t passed = pdTRUE;
    (void)argument;

    record(1U);
    xQueueSend(release_b, &token, 0U);
    record(2U);
    xQueueSend(release_c, &token, 0U);
    record(5U);

    for (index = 0U; index < 5U; index++)
    {
        if (trace_values[index] != expected[index]) passed = pdFALSE;
    }

    uart_puts("threshold_nested_preemption=");
    uart_puts(passed == pdTRUE ? "PASS" : "FAIL");
    uart_puts(" trace=");
    for (index = 0U; index < 5U; index++) uart_puthex(trace_values[index]);
    uart_puts("\r\n");

    for (;;) vTaskDelay(pdMS_TO_TICKS(1000U));
}

void main(void)
{
    TaskHandle_t task_a_handle = NULL;

    gic_init();
    uart_init();
    release_b = xQueueCreate(1U, sizeof(uint32_t));
    release_c = xQueueCreate(1U, sizeof(uint32_t));

    uart_puts("\r\nFreeRTOS threshold nested-preemption semantic test\r\n");
    xTaskCreate(task_c, "C", 512U, 0, TASK_C_PRIORITY, 0);
    xTaskCreate(task_b, "B", 512U, 0, TASK_B_PRIORITY, 0);
    xTaskCreate(task_a, "A", 512U, 0, TASK_A_PRIORITY, &task_a_handle);
    vTaskPreemptionThresholdSet(task_a_handle, TASK_B_PRIORITY);
    vTaskStartScheduler();
    for (;;) __asm volatile ("wfi");
}

void vApplicationIdleHook(void) {}
void vApplicationTickHook(void) {}
