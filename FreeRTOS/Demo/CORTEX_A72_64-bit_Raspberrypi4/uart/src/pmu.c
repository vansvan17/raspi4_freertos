/* pmu.c */
#include "pmu.h"

uint32_t pmu_counter_count(void)
{
    uint64_t pmcr;
    __asm volatile ("mrs %0, pmcr_el0" : "=r" (pmcr));
    return (uint32_t)((pmcr >> 11) & 0x1FU);
}

static void pmu_set_type(uint32_t counter, uint64_t type)
{
    switch (counter)
    {
    case 0: __asm volatile ("msr pmevtyper0_el0, %0" :: "r" (type)); break;
    case 1: __asm volatile ("msr pmevtyper1_el0, %0" :: "r" (type)); break;
    case 2: __asm volatile ("msr pmevtyper2_el0, %0" :: "r" (type)); break;
    case 3: __asm volatile ("msr pmevtyper3_el0, %0" :: "r" (type)); break;
    default: __asm volatile ("msr pmevtyper4_el0, %0" :: "r" (type)); break;
    }
}

void pmu_init(const uint32_t *events, uint32_t count)
{
    uint64_t enable = (1ULL << 31); /* cycle counter */
    uint32_t i;

    if (count > PMU_NUM_EVENTS) count = PMU_NUM_EVENTS;
    if (count > pmu_counter_count()) count = pmu_counter_count();

    /* Count at EL1 and EL0 only (filter bits zero), events as requested. */
    __asm volatile ("msr pmccfiltr_el0, %0" :: "r" (0ULL));
    for (i = 0; i < count; i++)
    {
        pmu_set_type(i, events[i]);
        enable |= (1ULL << i);
    }

    /* E=1 enable, P=1 reset events, C=1 reset cycles, LC=1 64-bit cycles. */
    __asm volatile ("msr pmcr_el0, %0" :: "r" ((1ULL << 0) | (1ULL << 1) | (1ULL << 2) | (1ULL << 6)));
    __asm volatile ("msr pmcntenset_el0, %0" :: "r" (enable));
    __asm volatile ("isb");
}
