# Multicore interference experiment

Status: runs on hardware through the RAM-log path (first runs 9 Nov 2026, in
`results/2026-11-09/`, from an uncommitted build). Final numbers need runs from
a committed build; the expectations below are still to be tested.

## Question

FreeRTOS runs on core 0 only. Cores 1-3 still share the 1 MiB L2 cache and
the DRAM controller with it. How much do they slow down a real-time task on
core 0, which hardware resource causes it, and do two known fixes help?

- **Cache coloring.** The L2 has 1024 sets indexed by physical address bits
  [15:6]. Bits [15:12] give 16 page colors. Victim pages use colors 0-4 and
  hog pages use colors 5-15, so they cannot evict each other from L2. Each
  color is 64 KiB of L2, so the 256 KiB victim gets 320 KiB: with exactly 4
  colors it had no headroom and thrashed against core 0's own stack and code
  whenever the cache state was unlucky (see `results/2026-11-09/`,
  `interference-replay-4col.log` vs `-5col.log`, same config order).
  Override with `-DVICTIM_COLORS=` / `-DHOG_COLORS=`; replay a run's order with
  `-DINTERFERENCE_SEED=<seed from its header>`.
- **Bandwidth throttling (MemGuard-style).** Each hog core reads its own L2
  refill counter and spins until the next 1 ms window once it uses its budget.

## What the image does

`make interference-test CROSS=aarch64-elf-` builds `kernel8.img`.

1. Core 0 turns on the MMU with write-back caches, then wakes cores 1-3 by
   writing `_boot` to the firmware spin table (`0xe0`, `0xe8`, `0xf0`).
2. Cores 1-3 drop to EL1, enable their MMU with core 0's page tables, and wait
   for a hog configuration.
3. Core 0 runs 33 configurations in a shuffled order (seed is printed):

   | Factor | Values |
   | --- | --- |
   | hog | idle, compute (no memory), read stream, write stream, pointer chase |
   | layout | shared, colored, uncached victim |
   | budget (L2 refills per ms per hog) | none, 4000, 1000, 250 (memory hogs only) |

4. For each configuration it times 4000 victim samples. A sample is one pointer
   chase over a 256 KiB working set (fits in L2, not in L1), with IRQs masked,
   timed with the cycle counter (`PMCCNTR_EL0`). There is a 200 us gap between
   samples so the hogs can evict the victim's lines, as they would between
   periods of a real control task.
5. It prints one `interference ...` line per configuration: cycle percentiles
   (min, p50, p90, p99, p99.9, max, mean), victim PMU counts per sample, hog
   work done, hog L2 refills, throttled windows, and the cycles and timer ticks
   elapsed. Cycles divided by ticks gives the real core clock, which catches
   DVFS or thermal throttling.

The header line records the git revision, whether the worktree was dirty, MIDR,
SCTLR, the L1 and L2 cache geometry read from `CCSIDR_EL1`, and the counter
frequency, so every log carries its own build and hardware metadata.

Options: `INTERFERENCE_SAMPLES=`, `VICTIM_BYTES=`, `HOG_BYTES=` (as
`EXTRA_CFLAGS` defines), `INTERFERENCE_RAW=1` to dump every sample, and
`RAM_LOG=1` for the RAM-log path (resets back to Linux at the end).

## Analysis

```sh
python3 tools/analyze_interference.py logs/interference-*.log --csv summary.csv
```

The analyzer rejects logs that are truncated, come from a dirty build, mix
builds, or ran with fewer than three hogs ready. It reports, per configuration,
the median p50 and p99 across runs, the p99 spread, p99 relative to the idle
case for the same layout, victim L2 refills, the measured core clock, hog
bandwidth, and throttled windows.

## Expectations to test, not results

- Uncached victim is far slower than cached, by roughly the DRAM to L2 latency
  ratio. This matters because every other image in this repo runs with the MMU
  off, so their data accesses are uncached.
- Compute hogs barely move the victim. Read, write, and chase hogs raise p99
  in the shared layout.
- Coloring removes most of the L2-eviction part. What remains is DRAM and
  interconnect contention, which throttling should reduce.
- If coloring changes nothing, the L2 may hash its set index, and that is a
  finding in its own right.

## Unvalidated assumptions

Check these on the first boot, in this order:

1. `hogs_alive=0x3`. If 0, the spin table addresses or the EL2 entry are wrong.
2. `sctlr` has bits 0, 2 and 12 set (MMU, D-cache, I-cache).
3. `ccsidr_l2` decodes to 1024 sets, 16 ways, 64-byte lines.
4. `cpu_mhz` is stable across configurations. If not, fix the clock in
   `config.txt` (for example `arm_freq=1500` with `force_turbo=1`) and add
   cooling.
5. Physical memory 0x10000000-0x14200000 is not used by the firmware.
6. The firmware's armstub sets `CPUECTLR_EL1.SMPEN` on every core. Without it
   the cores are not cache coherent and the hog handshake would hang (shows up
   as `hogs_ready` below 3).

## Files

- `FreeRTOS/Demo/CORTEX_A72_64-bit_Raspberrypi4/uart/src/interference_test.c`
- `FreeRTOS/Demo/CORTEX_A72_64-bit_Raspberrypi4/uart/src/pmu.c`, `pmu.h`
- `FreeRTOS/Demo/CORTEX_A72_64-bit_Raspberrypi4/uart/src/startup.S`: released
  cores now drop to EL1, and both paths open EL1 access to the PMU and counter
- `FreeRTOS/Demo/CORTEX_A72_64-bit_Raspberrypi4/uart/src/raspberrypi4.ld`:
  `.pt` is NOLOAD so the page tables do not add 4 MiB of zeros to the image
- `tools/analyze_interference.py`
