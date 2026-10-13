#include <stdint.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "board.h"
#include "uart.h"

#define GPIO_BASE_ADDR 0xFE200000UL
#define GPFSEL4_REG (*(volatile uint32_t *)(GPIO_BASE_ADDR + 0x10U))
#define GPSET1_REG  (*(volatile uint32_t *)(GPIO_BASE_ADDR + 0x20U))
#define GPCLR1_REG  (*(volatile uint32_t *)(GPIO_BASE_ADDR + 0x2CU))
#define ACT_LED_BIT (1U << (42U - 32U))

#define SENSOR_PERIOD_MS 10U
#define WATCHDOG_PERIOD_MS 100U
#define STATUS_PERIOD_MS 1000U
#define SETPOINT 1000

/*
 * Build the same workload with one of these policies:
 *   0: ordinary fixed-priority preemption
 *   1: raise/restore the controller priority around the compute phase
 *   2: preemption threshold (controller priority 3, threshold 4)
 */
#ifndef BENCHMARK_MODE
#define BENCHMARK_MODE 2
#endif

#define CONTROLLER_PRIORITY 3U
#define INTERFERER_PRIORITY 4U
#define URGENT_PRIORITY 5U
#define CONTROLLER_WORK_ITERATIONS 12000U
#define INTERFERER_WORK_ITERATIONS 4000U

typedef struct
{
    int32_t measurement;
    TickType_t sampled_at;
} SensorSample;

typedef struct
{
    int32_t command;
    TickType_t calculated_at;
} ControlCommand;

static QueueHandle_t sensor_queue;
static QueueHandle_t actuator_queue;
static volatile int32_t simulated_plant;
static volatile int32_t applied_command;
static volatile uint32_t sensor_runs;
static volatile uint32_t controller_runs;
static volatile uint32_t actuator_runs;
static volatile uint32_t missed_deadlines;
static volatile uint32_t interferer_runs;
static volatile uint64_t controller_latency_min = UINT64_MAX;
static volatile uint64_t controller_latency_max;
static volatile uint64_t controller_latency_sum;
static volatile uint64_t urgent_lateness_max;

static inline uint64_t counter_read(void)
{
    uint64_t value;
    __asm volatile ("isb\n\tmrs %0, cntpct_el0" : "=r" (value));
    return value;
}

static inline uint64_t counter_frequency(void)
{
    uint64_t value;
    __asm volatile ("mrs %0, cntfrq_el0" : "=r" (value));
    return value;
}

static uint64_t counter_to_us(uint64_t ticks)
{
    return (ticks * 1000000ULL) / counter_frequency();
}

static void deterministic_work(uint32_t iterations)
{
    volatile uint32_t state = 0x9e3779b9U;
    uint32_t i;

    for (i = 0; i < iterations; i++)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
    }
}

static int32_t clamp(int32_t value, int32_t lower, int32_t upper)
{
    if (value < lower) return lower;
    if (value > upper) return upper;
    return value;
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

static void led_init(void)
{
    uint32_t value = GPFSEL4_REG;
    value &= ~(7U << 6U);
    value |= (1U << 6U);
    GPFSEL4_REG = value;
    GPSET1_REG = ACT_LED_BIT;
}

/* Priority 4: deterministic 100 Hz sensor acquisition. */
static void sensor_task(void *argument)
{
    TickType_t wake = xTaskGetTickCount();
    SensorSample sample;
    (void)argument;

    for (;;)
    {
        sample.measurement = simulated_plant;
        sample.sampled_at = xTaskGetTickCount();
        xQueueOverwrite(sensor_queue, &sample);
        sensor_runs++;
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(SENSOR_PERIOD_MS));
    }
}

/* Priority 3: event-driven proportional controller. */
static void controller_task(void *argument)
{
    SensorSample sample;
    ControlCommand output;
    (void)argument;

    for (;;)
    {
        if (xQueueReceive(sensor_queue, &sample, portMAX_DELAY) == pdPASS)
        {
            uint64_t started = counter_read();
            uint64_t elapsed;
#if (BENCHMARK_MODE == 1)
            vTaskPrioritySet(NULL, INTERFERER_PRIORITY);
#endif
            deterministic_work(CONTROLLER_WORK_ITERATIONS);
            int32_t error = SETPOINT - sample.measurement;
            output.command = clamp(sample.measurement + (error / 2), 0, SETPOINT);
            output.calculated_at = xTaskGetTickCount();
            xQueueOverwrite(actuator_queue, &output);
#if (BENCHMARK_MODE == 1)
            vTaskPrioritySet(NULL, CONTROLLER_PRIORITY);
#endif
            elapsed = counter_read() - started;
            if (elapsed < controller_latency_min) controller_latency_min = elapsed;
            if (elapsed > controller_latency_max) controller_latency_max = elapsed;
            controller_latency_sum += elapsed;
            controller_runs++;
        }
    }
}

/* Priority 4: periodic background work that competes with the controller. */
static void interferer_task(void *argument)
{
    TickType_t wake = xTaskGetTickCount();
    (void)argument;

    for (;;)
    {
        deterministic_work(INTERFERER_WORK_ITERATIONS);
        interferer_runs++;
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(3U));
    }
}

/* Priority 2: applies controller output and simulates plant response. */
static void actuator_task(void *argument)
{
    ControlCommand output;
    (void)argument;

    for (;;)
    {
        if (xQueueReceive(actuator_queue, &output, pdMS_TO_TICKS(20)) == pdPASS)
        {
            applied_command = output.command;
            simulated_plant += (applied_command - simulated_plant) / 8;
            actuator_runs++;
        }
        else
        {
            missed_deadlines++;
        }
    }
}

/* Priority 5: checks task progress and provides the visible health signal. */
static void watchdog_task(void *argument)
{
    TickType_t wake = xTaskGetTickCount();
    uint64_t expected = counter_read();
    uint32_t previous_sensor_runs = 0;
    uint32_t heartbeat_divider = 0;
    (void)argument;

    for (;;)
    {
        uint64_t now = counter_read();
        uint64_t period = (counter_frequency() * WATCHDOG_PERIOD_MS) / 1000U;
        uint64_t lateness = (now > expected) ? (now - expected) : 0U;
        if (lateness > urgent_lateness_max) urgent_lateness_max = lateness;
        expected += period;

        if (sensor_runs == previous_sensor_runs)
        {
            missed_deadlines++;
        }
        previous_sensor_runs = sensor_runs;

        /* Healthy system: one short ACT LED pulse every 500 ms. */
        heartbeat_divider = (heartbeat_divider + 1U) % 5U;
        if (heartbeat_divider == 0U) GPCLR1_REG = ACT_LED_BIT;
        else GPSET1_REG = ACT_LED_BIT;

        vTaskDelayUntil(&wake, pdMS_TO_TICKS(WATCHDOG_PERIOD_MS));
    }
}

/* Priority 1: optional instrumentation for a future UART adapter. */
static void status_task(void *argument)
{
    TickType_t wake = xTaskGetTickCount();
    (void)argument;

    for (;;)
    {
        uart_puts("plant=0x");
        uart_puthex((uint64_t)simulated_plant);
        uart_puts(" command=0x");
        uart_puthex((uint64_t)applied_command);
        uart_puts(" cycles=0x");
        uart_puthex((uint64_t)actuator_runs);
        uart_puts(" missed=0x");
        uart_puthex((uint64_t)missed_deadlines);
        uart_puts(" mode=0x");
        uart_puthex((uint64_t)BENCHMARK_MODE);
        uart_puts(" ctrl_min_us=0x");
        uart_puthex(counter_to_us(controller_latency_min));
        uart_puts(" ctrl_max_us=0x");
        uart_puthex(counter_to_us(controller_latency_max));
        uart_puts(" ctrl_avg_us=0x");
        uart_puthex(controller_runs == 0U ? 0U : counter_to_us(controller_latency_sum / controller_runs));
        uart_puts(" urgent_max_us=0x");
        uart_puthex(counter_to_us(urgent_lateness_max));
        uart_puts(" interference=0x");
        uart_puthex((uint64_t)interferer_runs);
        uart_puts("\r\n");
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(STATUS_PERIOD_MS));
    }
}

void main(void)
{
    TaskHandle_t controller_handle = NULL;

    gic_init();
    led_init();
    uart_init();

    sensor_queue = xQueueCreate(1, sizeof(SensorSample));
    actuator_queue = xQueueCreate(1, sizeof(ControlCommand));

    uart_puts("\r\nFreeRTOS Raspberry Pi 4 real-time control demo\r\n");
    uart_puts("sensor=100Hz, controller=event-driven, watchdog=10Hz\r\n");
    uart_puts("benchmark modes: 0=baseline 1=priority-raise 2=threshold\r\n");
    uart_puts("selected mode=0x");
    uart_puthex((uint64_t)BENCHMARK_MODE);
    uart_puts("\r\n");

    xTaskCreate(watchdog_task, "watchdog", 512, 0, URGENT_PRIORITY, 0);
    xTaskCreate(sensor_task, "sensor", 512, 0, INTERFERER_PRIORITY, 0);
    xTaskCreate(interferer_task, "interferer", 512, 0, INTERFERER_PRIORITY, 0);
    xTaskCreate(controller_task, "controller", 512, 0, CONTROLLER_PRIORITY, &controller_handle);
#if (BENCHMARK_MODE == 2)
    vTaskPreemptionThresholdSet(controller_handle, 4);
#else
    (void)controller_handle;
#endif
    xTaskCreate(actuator_task, "actuator", 512, 0, 2, 0);
    xTaskCreate(status_task, "status", 512, 0, 1, 0);
    vTaskStartScheduler();

    for (;;) __asm volatile ("wfi");
}

void vApplicationIdleHook(void) {}
void vApplicationTickHook(void) {}
