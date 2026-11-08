/* ramlog.c */
#include "ramlog.h"

/* BCM2711 power management block, used by Linux for watchdog reboots. */
#define PM_BASE      (0xFE100000UL)
#define PM_RSTC      (*(volatile uint32_t *)(PM_BASE + 0x1CU))
#define PM_RSTS      (*(volatile uint32_t *)(PM_BASE + 0x20U))
#define PM_WDOG      (*(volatile uint32_t *)(PM_BASE + 0x24U))
#define PM_PASSWORD  (0x5A000000U)
#define PM_RSTC_WRCFG_CLR        (0xFFFFFFCFU)
#define PM_RSTC_WRCFG_FULL_RESET (0x00000020U)
#define PM_RSTS_PARTITION_MASK   (0x00000555U)
#define PM_WDOG_TIME_MASK        (0x000FFFFFU)

#define ramlog_hdr  ((volatile struct ramlog_header *)RAMLOG_PRIMARY_BASE)
#define ramlog_data ((volatile uint8_t *)(RAMLOG_PRIMARY_BASE + RAMLOG_HEADER_SIZE))

static uint32_t ramlog_active;

void ramlog_begin(uint32_t kind)
{
    uint64_t seen = (ramlog_hdr->host_magic == RAMLOG_HOST_ARMED) ? ramlog_hdr->host_nonce : 0U;

    ramlog_hdr->fw_magic = 0U;
    ramlog_hdr->seen_nonce = seen;
    ramlog_hdr->state = RAMLOG_STATE_RUNNING;
    ramlog_hdr->length = 0U;
    ramlog_hdr->capacity = RAMLOG_CAPACITY;
    ramlog_hdr->checksum = 0U;
    ramlog_hdr->kind = kind;
    ramlog_hdr->reports = 0U;
    ramlog_hdr->dropped = 0U;
    __asm volatile ("dsb sy");
    ramlog_hdr->fw_magic = RAMLOG_FW_MAGIC;
    __asm volatile ("dsb sy");
    ramlog_active = 1U;
}

void ramlog_putc(uint8_t c)
{
    uint32_t length;

    if (!ramlog_active) return;
    length = ramlog_hdr->length;
    if (length < RAMLOG_CAPACITY)
    {
        ramlog_data[length] = c;
        ramlog_hdr->length = length + 1U;
    }
    else
    {
        ramlog_hdr->dropped++;
    }
}

void ramlog_note_report(void)
{
    if (ramlog_active) ramlog_hdr->reports++;
}

void ramlog_finish(void)
{
    if (!ramlog_active) return;
    ramlog_hdr->checksum = ramlog_fnv1a(ramlog_data, ramlog_hdr->length);
    __asm volatile ("dsb sy");
    ramlog_hdr->state = RAMLOG_STATE_COMPLETE;
    __asm volatile ("dsb sy");
    ramlog_active = 0U;
}

uint32_t ramlog_fnv1a(volatile const uint8_t *data, uint32_t length)
{
    uint32_t hash = 0x811C9DC5U;
    uint32_t i;

    for (i = 0; i < length; i++)
    {
        hash ^= data[i];
        hash *= 0x01000193U;
    }
    return hash;
}

static void pm_watchdog_arm(uint32_t ticks)
{
    PM_WDOG = PM_PASSWORD | (ticks & PM_WDOG_TIME_MASK);
    PM_RSTC = PM_PASSWORD | (PM_RSTC & PM_RSTC_WRCFG_CLR) | PM_RSTC_WRCFG_FULL_RESET;
    __asm volatile ("dsb sy");
}

/*
 * Arms (or re-arms) the hardware watchdog; one second is 1 << 16 ticks. The
 * counter is 20 bits, so anything over 15 s would wrap to a short timeout
 * (35 s became 3 s). Clamp instead; long runs must re-arm.
 */
#define PM_WDOG_MAX_SECONDS (15U)
void pm_watchdog_start(uint32_t seconds)
{
    if (seconds > PM_WDOG_MAX_SECONDS) seconds = PM_WDOG_MAX_SECONDS;
    pm_watchdog_arm(seconds << 16);
}

/* VideoCore mailbox 0, property channel 8. */
#define MBOX_BASE    (0xFE00B880UL)
#define MBOX_READ    (*(volatile uint32_t *)(MBOX_BASE + 0x00U))
#define MBOX_STATUS  (*(volatile uint32_t *)(MBOX_BASE + 0x18U))
#define MBOX_WRITE   (*(volatile uint32_t *)(MBOX_BASE + 0x20U))
#define MBOX_FULL    (0x80000000U)
#define MBOX_EMPTY   (0x40000000U)
#define TAG_SET_REBOOT_FLAGS (0x00038064U)

static volatile uint32_t mbox_buf[8] __attribute__((aligned(16)));

static void mbox_cache_flush(void)
{
    for (uintptr_t p = (uintptr_t)mbox_buf; p < (uintptr_t)mbox_buf + sizeof(mbox_buf); p += 64U)
        __asm volatile ("dc civac, %0" :: "r" (p) : "memory");
    __asm volatile ("dsb sy" ::: "memory");
}

/*
 * The firmware keeps the one-shot tryboot request as a reboot flag. A Linux
 * reboot clears it through the firmware; a bare watchdog reset does not, so
 * the next boot would load tryboot.txt (this image) again, forever. Clear it
 * the same way Linux does. Bounded polling: the reset happens regardless.
 */
static void __attribute__((unused)) clear_tryboot_flag(void)
{
    uint32_t spins;

    mbox_buf[0] = sizeof(mbox_buf);
    mbox_buf[1] = 0U;                       /* request */
    mbox_buf[2] = TAG_SET_REBOOT_FLAGS;
    mbox_buf[3] = 4U;                       /* value buffer size */
    mbox_buf[4] = 0U;                       /* request code */
    mbox_buf[5] = 0U;                       /* flags: none, so no tryboot */
    mbox_buf[6] = 0U;                       /* end tag */
    mbox_buf[7] = 0U;
    mbox_cache_flush();

    for (spins = 0U; (MBOX_STATUS & MBOX_FULL) && spins < 1000000U; spins++) {}
    MBOX_WRITE = ((uint32_t)(uintptr_t)mbox_buf & ~0xFU) | 8U;
    for (spins = 0U; spins < 1000000U; spins++)
    {
        if (!(MBOX_STATUS & MBOX_EMPTY) && (MBOX_READ & 0xFU) == 8U) break;
    }
    mbox_cache_flush();
    /* Evidence for Linux: reply code (0x80000000 = ok) and tag response. */
    ramlog_hdr->reserved[0] = mbox_buf[1];
    ramlog_hdr->reserved[1] = mbox_buf[4];
    ramlog_hdr->reserved[2] = 0x464C4147U; /* "FLAG": clear attempted */
    __asm volatile ("dsb sy" ::: "memory");
}

/*
 * Off by default: the firmware clears the tryboot flag itself when it acts on
 * it (measured 9 Nov 2026: GET_REBOOT_FLAGS reads 0 inside a tryboot boot, and
 * a bare reset from there boots config.txt). Kept for diagnosis only.
 */
#ifndef RAMLOG_CLEAR_TRYBOOT_FLAG
#define RAMLOG_CLEAR_TRYBOOT_FLAG 0
#endif

void pm_watchdog_reset_now(void)
{
#if RAMLOG_CLEAR_TRYBOOT_FLAG
    clear_tryboot_flag();
#endif
    /* Boot partition 0, the normal config.txt, as `reboot` does in Linux. */
    PM_RSTS = PM_PASSWORD | (PM_RSTS & 0x00FFFFFFU & ~PM_RSTS_PARTITION_MASK);
    pm_watchdog_arm(10U);
    for (;;) __asm volatile ("wfi");
}
