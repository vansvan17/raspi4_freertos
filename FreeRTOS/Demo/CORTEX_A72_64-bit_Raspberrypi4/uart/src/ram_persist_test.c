#include <stdint.h>

#include "ramlog.h"

/*
 * Checks that DRAM survives the Linux -> firmware -> this image -> firmware
 * -> Linux round trip. For each candidate region it records the nonce Linux
 * left there, fills the data area with a pattern seeded from that nonce and
 * the region address, then resets. tools/pi_ramlog.py checks both halves.
 */

#define GPIO_BASE_ADDR 0xFE200000UL
#define GPFSEL4_REG (*(volatile uint32_t *)(GPIO_BASE_ADDR + 0x10U))
#define GPSET1_REG  (*(volatile uint32_t *)(GPIO_BASE_ADDR + 0x20U))
#define ACT_LED_BIT (1U << (42U - 32U))

volatile unsigned int benchmark_context_switches;
volatile uint32_t secondary_core_release;

void secondary_core_main(uint64_t core_id)
{
    (void)core_id;
    for (;;) __asm volatile ("wfe");
}

static void fill_region(uintptr_t base)
{
    volatile struct ramlog_header *hdr = (volatile struct ramlog_header *)base;
    volatile uint32_t *words = (volatile uint32_t *)(base + RAMLOG_HEADER_SIZE);
    uint64_t seen = (hdr->host_magic == RAMLOG_HOST_ARMED) ? hdr->host_nonce : 0U;
    uint32_t state = (uint32_t)(seen ^ (seen >> 32) ^ base) | 1U;
    uint32_t i;

    hdr->fw_magic = 0U;
    hdr->seen_nonce = seen;
    hdr->state = RAMLOG_STATE_RUNNING;
    hdr->kind = RAMLOG_KIND_PATTERN;
    hdr->capacity = RAMLOG_CAPACITY;
    hdr->reports = 0U;
    hdr->dropped = 0U;

    for (i = 0; i < RAMLOG_CAPACITY / 4U; i++)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        words[i] = state;
    }

    hdr->length = RAMLOG_CAPACITY;
    hdr->checksum = ramlog_fnv1a((volatile const uint8_t *)words, RAMLOG_CAPACITY);
    __asm volatile ("dsb sy");
    hdr->state = RAMLOG_STATE_COMPLETE;
    hdr->fw_magic = RAMLOG_FW_MAGIC;
    __asm volatile ("dsb sy");
}

void main(void)
{
    uint32_t value;

    /* If anything below hangs, the board still returns to Linux. */
    pm_watchdog_start(10U);

    value = GPFSEL4_REG;
    value &= ~(7U << 6U);
    value |= (1U << 6U);
    GPFSEL4_REG = value;
    GPSET1_REG = ACT_LED_BIT;

    fill_region(RAMLOG_PRIMARY_BASE);
    fill_region(RAMLOG_SECONDARY_BASE);

    pm_watchdog_reset_now();
}

void vApplicationIdleHook(void) {}
void vApplicationTickHook(void) {}
