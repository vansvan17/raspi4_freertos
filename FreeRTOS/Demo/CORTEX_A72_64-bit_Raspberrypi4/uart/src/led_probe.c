#include <stdint.h>
#include "ramlog.h"

/*
 * Boot probe for setups with no UART: blinks the green ACT LED at 1 Hz for
 * PROBE_BLINKS seconds, then resets to partition 0 (normal Linux boot) the
 * same way the RAM-log images do. Blinking proves the firmware ran our image;
 * Linux coming back afterwards proves the reset path works.
 */
#ifndef PROBE_BLINKS
#define PROBE_BLINKS 20U
#endif

#define GPIO_BASE_ADDR 0xFE200000UL
#define GPFSEL4_REG (*(volatile uint32_t *)(GPIO_BASE_ADDR + 0x10U))
#define GPSET1_REG  (*(volatile uint32_t *)(GPIO_BASE_ADDR + 0x20U))
#define GPCLR1_REG  (*(volatile uint32_t *)(GPIO_BASE_ADDR + 0x2CU))
#define ACT_LED_BIT (1U << (42U - 32U))

/* Breadcrumbs for Linux, see startup.S (BOOT_BREADCRUMBS). */
#define CRUMB ((volatile uint32_t *)0x300FF100UL)

volatile unsigned int benchmark_context_switches;
volatile uint32_t secondary_core_release;

void secondary_core_main(uint64_t core_id)
{
    (void)core_id;
    for (;;) __asm volatile ("wfe");
}

static uint64_t ticks(void)
{
    uint64_t value;
    __asm volatile ("isb\n\tmrs %0, cntpct_el0" : "=r" (value));
    return value;
}

static uint64_t frequency(void)
{
    uint64_t value;
    __asm volatile ("mrs %0, cntfrq_el0" : "=r" (value));
    return value;
}

void main(void)
{
    uint64_t half = frequency() / 2U;
    uint32_t value;

    CRUMB[0] = 0xBC000004U; /* stage 4: in main */
    CRUMB[3] = 0U;
    __asm volatile ("dsb sy" ::: "memory");
    value = GPFSEL4_REG;

    value &= ~(7U << 6U);
    value |= (1U << 6U);
    GPFSEL4_REG = value;

    for (uint32_t blink = 0; blink < PROBE_BLINKS; blink++)
    {
        uint64_t until = ticks() + half;
        pm_watchdog_start(15U); /* re-arm every blink; max is 15 s */
        GPSET1_REG = ACT_LED_BIT;
        while (ticks() < until) {}
        until = ticks() + half;
        GPCLR1_REG = ACT_LED_BIT;
        while (ticks() < until) {}
        CRUMB[3] = blink + 1U;
        __asm volatile ("dsb sy" ::: "memory");
    }
    CRUMB[0] = 0xBC000005U; /* stage 5: blinks done, resetting */
    __asm volatile ("dsb sy" ::: "memory");

    pm_watchdog_reset_now();
}

void vApplicationIdleHook(void) {}
void vApplicationTickHook(void) {}
