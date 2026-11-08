#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "board.h"
#include "mmu.h"
#include "mmu_cfg.h"
#include "pmu.h"
#include "ramlog.h"
#include "uart.h"

/*
 * Multicore interference on the shared Cortex-A72 L2 and DRAM.
 *
 * Core 0 runs FreeRTOS and times a fixed victim workload: one pointer chase
 * around a VICTIM_BYTES working set that fits in L2 but not in L1. Cores 1-3
 * run a hog that core 0 switches between configurations:
 *
 *   hog     idle, compute (no memory), read stream, write stream, pointer chase
 *   layout  shared   every buffer may use every L2 set
 *           colored  victim pages use colors 0-3, hog pages colors 4-15
 *           uncached victim buffer mapped non-cacheable (what the other
 *                    images do today, since they run with the MMU off)
 *   budget  0 for unlimited, else L2 refills a hog may cause per 1 ms window
 *           before it spins until the next window (MemGuard-style)
 *
 * The L2 is 1 MiB, 16-way, 64-byte lines, so 1024 sets indexed by physical
 * address bits [15:6]. Bits [15:12] are the page color. Everything here is
 * identity mapped, so coloring is just choosing which 4 KiB pages to touch.
 *
 * Each victim sample runs with IRQs masked so the tick does not land inside
 * it. Configurations run in a shuffled order seeded from the timer, so drift
 * over the run is not confounded with configuration.
 */

#ifndef RAM_LOG
#define RAM_LOG 0
#endif
#ifndef INTERFERENCE_SAMPLES
#define INTERFERENCE_SAMPLES 4000U
#endif
/* 1 also prints every sample's cycle count (large output). */
#ifndef INTERFERENCE_RAW
#define INTERFERENCE_RAW 0
#endif
#ifndef VICTIM_BYTES
#define VICTIM_BYTES (256U * 1024U)
#endif
#ifndef HOG_BYTES
#define HOG_BYTES (8U * 1024U * 1024U)
#endif
#ifndef BUILD_REV
#define BUILD_REV 0ULL
#endif
#ifndef BUILD_DIRTY
#define BUILD_DIRTY 1
#endif

#define SAMPLE_GAP_US 200U
#define SETTLE_MS 50U
#define WARMUP_PASSES 8U
#define THROTTLE_WINDOW_US 1000U
#define ACK_TIMEOUT_MS 3000U

#define LINE_BYTES 64U
#define PAGE_BYTES 4096U
#define LINES_PER_PAGE (PAGE_BYTES / LINE_BYTES)
#define NUM_COLORS 16U
#define COLOR_PERIOD (NUM_COLORS * PAGE_BYTES)
#define ALL_COLORS 0xFFFFU
/* Victim and hog page colors for the colored layout; they must not overlap.
 * Each color is 64 KiB of L2, so the 256 KiB victim needs more than 4 colors:
 * with exactly 4 it filled its partition, and core 0's own stack, sample
 * buffer and code in the same colors made it thrash (~950 L2 refills per
 * sample instead of ~50, depending on the previous config). 5 colors leave
 * 25% headroom (A/B replay of one config order, 9 Nov 2026). */
#ifndef VICTIM_COLORS
#define VICTIM_COLORS 0x001FU
#endif
#ifndef HOG_COLORS
#define HOG_COLORS 0xFFE0U
#endif

/* Physical layout. 0x0-0xA00000 holds the image, stacks and page tables. */
#define VICTIM_BASE 0x10000000UL
#define HOG_REGION_BASE 0x11000000UL
#define HOG_REGION_SPAN 0x01000000UL
#define UNCACHED_BASE 0x14000000UL

/* Pi 4 firmware parks cores 1-3 polling these addresses (cpu-release-addr). */
#define SPIN_TABLE_CORE1 0xE0UL

enum { HOG_IDLE, HOG_COMPUTE, HOG_READ, HOG_WRITE, HOG_CHASE };
enum { LAYOUT_SHARED, LAYOUT_COLORED, LAYOUT_UNCACHED };

volatile unsigned int benchmark_context_switches;
volatile uint32_t secondary_core_release;

extern void configure_mmu(void);
extern void flush_dcache_range(uintptr_t start, uintptr_t end);
extern char _boot[];

struct ptc_t pt_config[NUM_PT_CONFIGS] =
{
    { /* Image, data, stacks and page tables */
        .addr = 0x00000000ULL,
        .size = SIZE_8M + SIZE_2M,
        .executable = XN_OFF,
        .sharable = INNER_SHARABLE,
        .permission = READ_WRITE,
        .policy = TYPE_MEM_CACHE_WB,
    },
    { /* Victim and hog buffers */
        .addr = VICTIM_BASE,
        .size = SIZE_64M,
        .executable = XN_OFF,
        .sharable = INNER_SHARABLE,
        .permission = READ_WRITE,
        .policy = TYPE_MEM_CACHE_WB,
    },
    { /* Victim buffer for the uncached layout */
        .addr = UNCACHED_BASE,
        .size = SIZE_2M,
        .executable = XN_OFF,
        .sharable = OUTER_SHARABLE,
        .permission = READ_WRITE,
        .policy = TYPE_MEM_NONCACHE,
    },
    { /* Peripherals. RAM-log regions stay on the default Device mapping. */
        .addr = 0xFC000000ULL,
        .size = SIZE_64M,
        .executable = XN_OFF,
        .sharable = OUTER_SHARABLE,
        .permission = READ_WRITE,
        .policy = TYPE_DEVICE,
    },
    { 0 }, /* unused: zero size maps nothing */
};

/* Written by core 0 and cleaned to DRAM before cores 1-3 read it with their
 * MMU still off. */
static struct {
    uint64_t mair;
    uint64_t tcr;
    uint64_t ttbr0;
    uint64_t ttbr1;
    uint64_t sctlr;
} __attribute__((aligned(64))) mmu_boot;

static struct {
    volatile uint32_t generation;
    volatile uint32_t hog;
    volatile uint32_t layout;
    volatile uint32_t budget;
} __attribute__((aligned(64))) control;

struct hog_slot {
    volatile uint64_t units;     /* lines touched (compute: loop blocks) */
    volatile uint64_t refills;   /* this core's L2 refills */
    volatile uint64_t throttled; /* windows that ran out of budget */
    volatile uint32_t ack;       /* last generation this hog prepared for */
    volatile uint32_t alive;
} __attribute__((aligned(64)));

static struct hog_slot hogs[4];

struct buffer {
    uintptr_t base;
    uint32_t lines;
    uint32_t ncolors;
    uint8_t order[NUM_COLORS];
};

struct config {
    uint8_t hog;
    uint8_t layout;
    uint16_t budget;
};

static struct config configs[64];
static uint32_t config_count;
static uint32_t samples[INTERFERENCE_SAMPLES];
static volatile uintptr_t sink;

/*-----------------------------------------------------------*/

static void buffer_init(struct buffer *buf, uintptr_t base, uint32_t bytes, uint32_t colors)
{
    uint32_t c;

    buf->base = base;
    buf->lines = bytes / LINE_BYTES;
    buf->ncolors = 0;
    for (c = 0; c < NUM_COLORS; c++)
    {
        if (colors & (1U << c)) buf->order[buf->ncolors++] = (uint8_t)c;
    }
}

static uintptr_t page_address(const struct buffer *buf, uint32_t page)
{
    return buf->base + (uintptr_t)(page / buf->ncolors) * COLOR_PERIOD
        + (uintptr_t)buf->order[page % buf->ncolors] * PAGE_BYTES;
}

static uintptr_t line_address(const struct buffer *buf, uint32_t line)
{
    return page_address(buf, line / LINES_PER_PAGE) + (line % LINES_PER_PAGE) * LINE_BYTES;
}

/* Links every line into one cycle in an order the prefetchers cannot follow.
 * lines must be a power of two: x -> a*x + c (mod 2^k) with a = 1 (mod 4) and
 * odd c visits every value once. Returns the first line's address. */
static uintptr_t chase_build(const struct buffer *buf)
{
    uint32_t mask = buf->lines - 1U;
    uint32_t x = 0;
    uint32_t i;

    for (i = 0; i < buf->lines; i++)
    {
        uint32_t next = (1103515245U * x + 12345U) & mask;
        *(volatile uintptr_t *)line_address(buf, x) = line_address(buf, next);
        x = next;
    }
    __asm volatile ("dsb sy" ::: "memory");
    return line_address(buf, 0);
}

static uintptr_t chase(uintptr_t p, uint32_t hops)
{
    while (hops--) p = *(volatile uintptr_t *)p;
    return p;
}

static void spin_until(uint64_t deadline)
{
    while (timer_ticks() < deadline) {}
}

static uint64_t us_to_ticks(uint64_t us)
{
    return (timer_frequency() * us) / 1000000ULL;
}

/*-----------------------------------------------------------*/
/* Cores 1-3 */

static void secondary_mmu_enable(void)
{
    __asm volatile ("tlbi vmalle1\n\tdsb nsh\n\tic iallu\n\tdsb nsh\n\tisb" ::: "memory");
    __asm volatile ("msr mair_el1, %0" :: "r" (mmu_boot.mair));
    __asm volatile ("msr tcr_el1, %0" :: "r" (mmu_boot.tcr));
    __asm volatile ("msr ttbr0_el1, %0" :: "r" (mmu_boot.ttbr0));
    __asm volatile ("msr ttbr1_el1, %0" :: "r" (mmu_boot.ttbr1));
    __asm volatile ("isb");
    __asm volatile ("msr sctlr_el1, %0\n\tisb" :: "r" (mmu_boot.sctlr) : "memory");
}

static void hog_prepare(struct buffer *buf, uintptr_t *chase_head, uint64_t core_id,
                        uint32_t hog, uint32_t layout)
{
    uint32_t colors = (layout == LAYOUT_COLORED) ? HOG_COLORS : ALL_COLORS;

    buffer_init(buf, HOG_REGION_BASE + (core_id - 1U) * HOG_REGION_SPAN, HOG_BYTES, colors);
    if (hog == HOG_CHASE) *chase_head = chase_build(buf);
}

void secondary_core_main(uint64_t core_id)
{
    static const uint32_t events[] = { PMU_EV_L2D_CACHE_REFILL };
    struct hog_slot *slot = &hogs[core_id];
    struct buffer buf;
    uintptr_t cursor = 0;
    uint32_t page = 0;
    uint32_t seen = 0;
    uint32_t hog = HOG_IDLE;
    uint32_t budget = 0;
    uint64_t window = us_to_ticks(THROTTLE_WINDOW_US);
    uint64_t window_end = 0;
    uint32_t window_start = 0;
    uint32_t last_refills;
    uint64_t state = 0x9E3779B97F4A7C15ULL ^ core_id;
    uint64_t sum = 0;

    secondary_mmu_enable();
    pmu_init(events, 1);
    last_refills = pmu_event(0);
    slot->alive = 1U;

    for (;;)
    {
        uint32_t generation = control.generation;
        uint32_t j;

        if (generation != seen)
        {
            __asm volatile ("dmb ish" ::: "memory");
            budget = control.budget;
            hog = control.hog;
            hog_prepare(&buf, &cursor, core_id, hog, control.layout);
            page = 0;
            window_end = 0;
            seen = generation;
            __asm volatile ("dmb ish" ::: "memory");
            slot->ack = generation;
        }

        if (hog == HOG_IDLE)
        {
            __asm volatile ("wfe");
            continue;
        }

        if (budget != 0U)
        {
            uint64_t now = timer_ticks();
            if (now >= window_end)
            {
                window_end = now + window;
                window_start = pmu_event(0);
            }
            else if (pmu_event(0) - window_start >= budget)
            {
                slot->throttled++;
                spin_until(window_end);
                continue;
            }
        }

        switch (hog)
        {
        case HOG_COMPUTE:
            for (j = 0; j < LINES_PER_PAGE; j++)
            {
                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;
            }
            sum += state;
            break;
        case HOG_READ:
        {
            const volatile uint64_t *p = (const volatile uint64_t *)page_address(&buf, page);
            for (j = 0; j < LINES_PER_PAGE; j++) sum += p[j * (LINE_BYTES / 8U)];
            break;
        }
        case HOG_WRITE:
        {
            /* One word per line: each store has to fetch the line first. */
            volatile uint64_t *p = (volatile uint64_t *)page_address(&buf, page);
            for (j = 0; j < LINES_PER_PAGE; j++) p[j * (LINE_BYTES / 8U)] = sum + j;
            break;
        }
        default:
            cursor = chase(cursor, LINES_PER_PAGE);
            break;
        }
        page = (page + 1U) % (buf.lines / LINES_PER_PAGE);
        slot->units += LINES_PER_PAGE;
        {
            /* Widen the 32-bit event counter so long runs do not wrap. */
            uint32_t refills = pmu_event(0);
            slot->refills += refills - last_refills;
            last_refills = refills;
        }
    }
    (void)sum;
}

/*-----------------------------------------------------------*/
/* Core 0 */

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

static void release_secondaries(void)
{
    uintptr_t table;
    uint32_t core;

    /* Hide the low constant address from GCC's array-bounds checks. */
    __asm volatile ("" : "=r" (table) : "0" (SPIN_TABLE_CORE1));

    __asm volatile ("mrs %0, mair_el1" : "=r" (mmu_boot.mair));
    __asm volatile ("mrs %0, tcr_el1" : "=r" (mmu_boot.tcr));
    __asm volatile ("mrs %0, ttbr0_el1" : "=r" (mmu_boot.ttbr0));
    __asm volatile ("mrs %0, ttbr1_el1" : "=r" (mmu_boot.ttbr1));
    __asm volatile ("mrs %0, sctlr_el1" : "=r" (mmu_boot.sctlr));
    flush_dcache_range((uintptr_t)&mmu_boot, (uintptr_t)&mmu_boot + sizeof(mmu_boot));

    secondary_core_release = 1U;
    flush_dcache_range((uintptr_t)&secondary_core_release,
                       (uintptr_t)&secondary_core_release + sizeof(secondary_core_release));

    for (core = 1; core < 4U; core++)
    {
        *(volatile uint64_t *)(table + (core - 1U) * 8U) = (uint64_t)(uintptr_t)_boot;
    }
    flush_dcache_range(table, table + 3U * 8U);
    __asm volatile ("dsb sy\n\tsev" ::: "memory");
}

static void put_field(const char *name, uint64_t value)
{
    uart_puts(" ");
    uart_puts(name);
    uart_puts("=0x");
    uart_puthex(value);
}

static uint64_t ccsidr(uint64_t level)
{
    uint64_t value;
    __asm volatile ("msr csselr_el1, %0\n\tisb\n\tmrs %1, ccsidr_el1"
                    : "=r" (value) : "r" (level << 1) : "memory");
    return value;
}

static uint32_t hogs_alive(void)
{
    return hogs[1].alive + hogs[2].alive + hogs[3].alive;
}

static void build_configs(uint64_t seed)
{
    static const uint16_t budgets[] = { 0U, 4000U, 1000U, 250U };
    uint32_t layout, hog, b, i;

    config_count = 0;
    for (layout = LAYOUT_SHARED; layout <= LAYOUT_COLORED; layout++)
    {
        for (hog = HOG_IDLE; hog <= HOG_CHASE; hog++)
        {
            uint32_t nb = (hog >= HOG_READ) ? 4U : 1U;
            for (b = 0; b < nb; b++)
            {
                configs[config_count++] = (struct config){ (uint8_t)hog, (uint8_t)layout, budgets[b] };
            }
        }
    }
    for (hog = HOG_IDLE; hog <= HOG_CHASE; hog++)
    {
        configs[config_count++] = (struct config){ (uint8_t)hog, LAYOUT_UNCACHED, 0U };
    }

    /* Fisher-Yates with xorshift64. */
    seed |= 1U;
    for (i = config_count - 1U; i > 0U; i--)
    {
        struct config tmp;
        uint32_t j;
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        j = (uint32_t)(seed % (i + 1U));
        tmp = configs[i];
        configs[i] = configs[j];
        configs[j] = tmp;
    }
}

static uint32_t apply_config(const struct config *cfg, uint32_t generation)
{
    uint64_t deadline;

    control.hog = cfg->hog;
    control.layout = cfg->layout;
    control.budget = cfg->budget;
    __asm volatile ("dmb ish" ::: "memory");
    control.generation = generation;
    __asm volatile ("dsb ish\n\tsev" ::: "memory");

    deadline = timer_ticks() + us_to_ticks(ACK_TIMEOUT_MS * 1000ULL);
    for (;;)
    {
        uint32_t ready = 0, core;
        for (core = 1; core < 4U; core++)
        {
            if (hogs[core].alive && hogs[core].ack == generation) ready++;
        }
        if (ready == hogs_alive()) return ready;
        if (timer_ticks() >= deadline) return ready;
        vTaskDelay(1);
    }
}

static void sort_samples(uint32_t *values, uint32_t count)
{
    static const uint32_t gaps[] = { 701U, 301U, 132U, 57U, 23U, 10U, 4U, 1U };
    uint32_t g, i;

    for (g = 0; g < sizeof(gaps) / sizeof(gaps[0]); g++)
    {
        uint32_t gap = gaps[g];
        for (i = gap; i < count; i++)
        {
            uint32_t v = values[i];
            uint32_t j = i;
            while (j >= gap && values[j - gap] > v)
            {
                values[j] = values[j - gap];
                j -= gap;
            }
            values[j] = v;
        }
    }
}

static uint32_t percentile(const uint32_t *sorted, uint32_t count, uint32_t permille)
{
    return sorted[((uint64_t)(count - 1U) * permille) / 1000U];
}

static void run_config(uint32_t index, const struct config *cfg, uint32_t generation)
{
    struct buffer victim;
    uint64_t events[PMU_NUM_EVENTS] = { 0 };
    uint64_t hog_units[4], hog_refills[4], hog_throttled[4];
    uint64_t cycles_sum = 0, start_cycles, start_ticks, elapsed_cycles, elapsed_ticks;
    uint64_t gap = us_to_ticks(SAMPLE_GAP_US);
    uintptr_t head;
    uint32_t ready, hops, i, e, core;

    ready = apply_config(cfg, generation);

    if (cfg->layout == LAYOUT_UNCACHED)
        buffer_init(&victim, UNCACHED_BASE, VICTIM_BYTES, ALL_COLORS);
    else
        buffer_init(&victim, VICTIM_BASE, VICTIM_BYTES,
                    (cfg->layout == LAYOUT_COLORED) ? VICTIM_COLORS : ALL_COLORS);
    head = chase_build(&victim);
    hops = victim.lines;
    for (i = 0; i < WARMUP_PASSES; i++) sink ^= chase(head, hops);
    vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));

    for (core = 1; core < 4U; core++)
    {
        hog_units[core] = hogs[core].units;
        hog_refills[core] = hogs[core].refills;
        hog_throttled[core] = hogs[core].throttled;
    }
    start_cycles = pmu_cycles();
    start_ticks = timer_ticks();

    for (i = 0; i < INTERFERENCE_SAMPLES; i++)
    {
        struct pmu_snapshot before, after;

        portDISABLE_INTERRUPTS();
        pmu_snapshot_take(&before);
        sink ^= chase(head, hops);
        pmu_snapshot_take(&after);
        portENABLE_INTERRUPTS();

        samples[i] = (uint32_t)(after.cycles - before.cycles);
        cycles_sum += samples[i];
        for (e = 0; e < PMU_NUM_EVENTS; e++) events[e] += (uint32_t)(after.events[e] - before.events[e]);

        spin_until(timer_ticks() + gap);
    }

    elapsed_cycles = pmu_cycles() - start_cycles;
    elapsed_ticks = timer_ticks() - start_ticks;
    for (core = 1; core < 4U; core++)
    {
        hog_units[core] = hogs[core].units - hog_units[core];
        hog_refills[core] = hogs[core].refills - hog_refills[core];
        hog_throttled[core] = hogs[core].throttled - hog_throttled[core];
    }

#if (INTERFERENCE_RAW == 1)
    uart_puts("raw");
    put_field("cfg", index);
    uart_puts(" cycles=");
    for (i = 0; i < INTERFERENCE_SAMPLES; i++)
    {
        uart_puthex(samples[i]);
        uart_puts(i + 1U < INTERFERENCE_SAMPLES ? "," : "\r\n");
    }
#endif

    sort_samples(samples, INTERFERENCE_SAMPLES);

    uart_puts("interference");
    put_field("cfg", index);
    put_field("hog", cfg->hog);
    put_field("layout", cfg->layout);
    put_field("budget", cfg->budget);
    put_field("hogs_ready", ready);
    put_field("n", INTERFERENCE_SAMPLES);
    put_field("hops", hops);
    put_field("min", samples[0]);
    put_field("p50", percentile(samples, INTERFERENCE_SAMPLES, 500U));
    put_field("p90", percentile(samples, INTERFERENCE_SAMPLES, 900U));
    put_field("p99", percentile(samples, INTERFERENCE_SAMPLES, 990U));
    put_field("p999", percentile(samples, INTERFERENCE_SAMPLES, 999U));
    put_field("max", samples[INTERFERENCE_SAMPLES - 1U]);
    put_field("mean", cycles_sum / INTERFERENCE_SAMPLES);
    put_field("inst", events[1] / INTERFERENCE_SAMPLES);
    put_field("l1d_refill", events[0] / INTERFERENCE_SAMPLES);
    put_field("l2d", events[2] / INTERFERENCE_SAMPLES);
    put_field("l2d_refill", events[3] / INTERFERENCE_SAMPLES);
    put_field("bus", events[4] / INTERFERENCE_SAMPLES);
    put_field("elapsed_cycles", elapsed_cycles);
    put_field("elapsed_ticks", elapsed_ticks);
    put_field("hog1_units", hog_units[1]);
    put_field("hog2_units", hog_units[2]);
    put_field("hog3_units", hog_units[3]);
    put_field("hog_refills", hog_refills[1] + hog_refills[2] + hog_refills[3]);
    put_field("hog_throttled", hog_throttled[1] + hog_throttled[2] + hog_throttled[3]);
    uart_puts("\r\n");
}

static void experiment_task(void *argument)
{
    static const uint32_t events[PMU_NUM_EVENTS] = {
        PMU_EV_L1D_CACHE_REFILL, PMU_EV_INST_RETIRED, PMU_EV_L2D_CACHE,
        PMU_EV_L2D_CACHE_REFILL, PMU_EV_BUS_ACCESS,
    };
#ifdef INTERFERENCE_SEED
    uint64_t seed = INTERFERENCE_SEED; /* replay a previous run's config order */
#else
    uint64_t seed = timer_ticks();
#endif
    uint64_t midr, sctlr;
    uint64_t deadline;
    uint32_t i;
    (void)argument;

    pmu_init(events, PMU_NUM_EVENTS);

    deadline = timer_ticks() + us_to_ticks(1000000ULL);
    while (hogs_alive() < 3U && timer_ticks() < deadline) vTaskDelay(1);

    build_configs(seed);

    __asm volatile ("mrs %0, midr_el1" : "=r" (midr));
    __asm volatile ("mrs %0, sctlr_el1" : "=r" (sctlr));
    uart_puts("\r\ninterference-test start");
    put_field("rev", BUILD_REV);
    put_field("dirty", BUILD_DIRTY);
    put_field("midr", midr);
    put_field("sctlr", sctlr);
    put_field("ccsidr_l1d", ccsidr(0));
    put_field("ccsidr_l2", ccsidr(1));
    put_field("pmu_n", pmu_counter_count());
    put_field("cntfrq", timer_frequency());
    put_field("hogs_alive", hogs_alive());
    put_field("victim_bytes", VICTIM_BYTES);
    put_field("hog_bytes", HOG_BYTES);
    put_field("victim_colors", VICTIM_COLORS);
    put_field("hog_colors", HOG_COLORS);
    put_field("gap_us", SAMPLE_GAP_US);
    put_field("window_us", THROTTLE_WINDOW_US);
    put_field("seed", seed);
    put_field("configs", config_count);
    uart_puts("\r\n");
#if (RAM_LOG == 1)
    ramlog_note_report();
#endif

    for (i = 0; i < config_count; i++)
    {
#if (RAM_LOG == 1)
        pm_watchdog_start(15U);
#endif
        run_config(i, &configs[i], i + 1U);
#if (RAM_LOG == 1)
        ramlog_note_report();
#endif
    }

    /* Park the hogs so the end of the log is not taken under load. */
    control.hog = HOG_IDLE;
    __asm volatile ("dmb ish" ::: "memory");
    control.generation = config_count + 1U;
    __asm volatile ("dsb ish\n\tsev" ::: "memory");

    uart_puts("interference-test complete");
    put_field("configs", config_count);
    put_field("sink", sink & 1U);
    uart_puts("\r\n");
#if (RAM_LOG == 1)
    uart_puts("ramlog complete\r\n");
    ramlog_finish();
    pm_watchdog_reset_now();
#endif

    for (;;) vTaskDelay(portMAX_DELAY);
}

void main(void)
{
#if (RAM_LOG == 1)
    pm_watchdog_start(15U);
    ramlog_begin(RAMLOG_KIND_LOG);
#endif
    configure_mmu();
    release_secondaries();

    gic_init();
    uart_init();

    xTaskCreate(experiment_task, "experiment", 1024, 0, tskIDLE_PRIORITY + 2U, 0);
    vTaskStartScheduler();

    for (;;) __asm volatile ("wfi");
}

void vApplicationIdleHook(void) {}
void vApplicationTickHook(void) {}
