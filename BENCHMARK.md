# Raspberry Pi 4 scheduling benchmark

This benchmark runs the same synthetic control workload under three scheduling
policies. Its purpose is to measure the tradeoff discussed in the FreeRTOS
community thread rather than assume that preemption thresholds are beneficial.

## Policies

Build from `FreeRTOS/Demo/CORTEX_A72_64-bit_Raspberrypi4/uart`:

```sh
make benchmark-baseline CROSS=aarch64-elf-
make benchmark-ceiling CROSS=aarch64-elf-
make benchmark-threshold CROSS=aarch64-elf-
make benchmark-suspend CROSS=aarch64-elf-
```

The modes are:

| Mode | Policy |
| --- | --- |
| 0 | Normal fixed-priority preemption |
| 1 | Explicit priority-ceiling style raise from priority 3 to 4, then restore it |
| 2 | Keep priority 3 and apply preemption threshold 4 |
| 3 | Suspend and resume the scheduler around controller computation |

Each build produces `kernel8.img`. Keep the firmware, workload size, clock
configuration, and observation interval identical when comparing modes.

## Workload

- Sensor task: priority 4, period 10 ms
- Controller task: priority 3, event driven, fixed compute loop
- Interference task: priority 4, period 3 ms
- Actuator task: priority 2
- Urgent watchdog: priority 5, period 100 ms
- UART reporter: priority 1, period 1 s

The priority-4 interference task creates repeatable preemption pressure while
the priority-5 watchdog checks that urgent work is not delayed excessively.

## Output

The UART report contains hexadecimal values:

- `mode`: selected scheduling policy
- `ctrl_min_us`, `ctrl_max_us`, `ctrl_avg_us`: controller compute latency
- `urgent_max_us`: maximum watchdog release lateness
- `interference`: number of completed interference jobs
- `switches`: cumulative scheduler context switches
- `missed`: observed control or watchdog progress failures

Collect at least 60 seconds per mode after a 10 second warm-up. Run modes in a
rotating order to reduce thermal and clock bias. The first comparison should
focus on maximum controller latency, urgent-task lateness, context switches,
and missed deadlines. Scheduler suspension is expected to reduce preemption but
may increase urgent-task lateness, which is precisely the tradeoff being tested.

Save each UART capture separately, then generate a comparison table:

```sh
python3 tools/analyze_benchmark.py \
  results/baseline.log results/ceiling.log \
  results/threshold.log results/suspend.log \
  --output results/summary.csv
```

The parser discards the first ten one-second reports by default and rejects
incomplete logs or files containing mixed benchmark modes.

## Important semantic test

The benchmark must also exercise this sequence:

1. Task A runs at priority 3 with threshold 4.
2. Task B becomes ready at priority 4 and is deferred.
3. Task C becomes ready at priority 5 and preempts A.
4. C blocks while A and B remain ready.

At step 4, B is allowed to run because A's threshold applies only while A is
executing. A threshold is therefore not mutual exclusion and does not guarantee
that B remains deferred until A finishes. Applications needing that guarantee
must use synchronization or a real priority change.

## Current multicore status

This Raspberry Pi 4 port is currently single-core. `startup.S` boots core 0 and
parks cores 1 through 3, and the bundled FreeRTOS kernel predates the current SMP
kernel. The benchmark is valid only as a single-core experiment until secondary
core startup, per-core interrupt setup, inter-processor interrupts, scheduler
locking, and the current FreeRTOS SMP kernel are integrated and tested.

