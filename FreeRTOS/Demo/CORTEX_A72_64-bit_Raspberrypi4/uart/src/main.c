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
            int32_t error = SETPOINT - sample.measurement;
            output.command = clamp(sample.measurement + (error / 2), 0, SETPOINT);
            output.calculated_at = xTaskGetTickCount();
            xQueueOverwrite(actuator_queue, &output);
            controller_runs++;
        }
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
    uint32_t previous_sensor_runs = 0;
    uint32_t heartbeat_divider = 0;
    (void)argument;

    for (;;)
    {
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
    uart_puts("limited preemption: controller priority=3 threshold=4\r\n");

    xTaskCreate(watchdog_task, "watchdog", 512, 0, 5, 0);
    xTaskCreate(sensor_task, "sensor", 512, 0, 4, 0);
    xTaskCreate(controller_task, "controller", 512, 0, 3, &controller_handle);
    vTaskPreemptionThresholdSet(controller_handle, 4);
    xTaskCreate(actuator_task, "actuator", 512, 0, 2, 0);
    xTaskCreate(status_task, "status", 512, 0, 1, 0);
    vTaskStartScheduler();

    for (;;) __asm volatile ("wfi");
}

void vApplicationIdleHook(void) {}
void vApplicationTickHook(void) {}
