/* pmu.h
 *
 * Cortex-A72 performance monitor access from EL1. startup.S sets MDCR_EL2 so
 * these registers do not trap to EL2. Each core has its own PMU, so every core
 * that wants counts must call pmu_init() itself.
 */
#ifndef PMU_H
#define PMU_H

#include <stdint.h>

/* ARMv8 common event numbers implemented by the Cortex-A72. */
#define PMU_EV_L1D_CACHE_REFILL (0x03U)
#define PMU_EV_INST_RETIRED     (0x08U)
#define PMU_EV_L2D_CACHE        (0x16U)
#define PMU_EV_L2D_CACHE_REFILL (0x17U)
#define PMU_EV_BUS_ACCESS       (0x19U)

#define PMU_NUM_EVENTS (5U)

struct pmu_snapshot {
    uint64_t cycles;
    uint32_t events[PMU_NUM_EVENTS];
};

static inline uint64_t pmu_cycles(void)
{
    uint64_t value;
    __asm volatile ("isb\n\tmrs %0, pmccntr_el0" : "=r" (value) :: "memory");
    return value;
}

static inline uint64_t timer_ticks(void)
{
    uint64_t value;
    __asm volatile ("isb\n\tmrs %0, cntpct_el0" : "=r" (value) :: "memory");
    return value;
}

static inline uint64_t timer_frequency(void)
{
    uint64_t value;
    __asm volatile ("mrs %0, cntfrq_el0" : "=r" (value));
    return value;
}

static inline uint32_t pmu_event(uint32_t counter)
{
    uint64_t value;
    switch (counter)
    {
    case 0: __asm volatile ("mrs %0, pmevcntr0_el0" : "=r" (value)); break;
    case 1: __asm volatile ("mrs %0, pmevcntr1_el0" : "=r" (value)); break;
    case 2: __asm volatile ("mrs %0, pmevcntr2_el0" : "=r" (value)); break;
    case 3: __asm volatile ("mrs %0, pmevcntr3_el0" : "=r" (value)); break;
    default: __asm volatile ("mrs %0, pmevcntr4_el0" : "=r" (value)); break;
    }
    return (uint32_t)value;
}

/* Straight-line reads so the overhead is identical before and after. */
static inline void pmu_snapshot_take(struct pmu_snapshot *snap)
{
    __asm volatile ("isb" ::: "memory");
    snap->events[0] = pmu_event(0);
    snap->events[1] = pmu_event(1);
    snap->events[2] = pmu_event(2);
    snap->events[3] = pmu_event(3);
    snap->events[4] = pmu_event(4);
    snap->cycles = pmu_cycles();
}

/* Number of event counters this core implements (PMCR_EL0.N). */
uint32_t pmu_counter_count(void);

/* Enables the 64-bit cycle counter and up to five event counters. Pass fewer
 * than five events to leave the rest unprogrammed. */
void pmu_init(const uint32_t *events, uint32_t count);

#endif /* PMU_H */
