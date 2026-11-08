# Raspberry Pi 4 scheduling benchmark

This benchmark runs the same synthetic control workload under four scheduling
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

Build a deterministic version of this sequence with:

```sh
make threshold-semantics CROSS=aarch64-elf-
```

On the Pi, the UART must print `threshold_nested_preemption=PASS` followed by
the trace values 1, 2, 3, 4, 5. This is a semantic check, not a performance
result.

## Ready-queue placement experiment

The community also suggested examining whether a preempted task resumes before
or after an already-ready task at the same priority. Build the experiment with:

```sh
make ready-queue-test CROSS=aarch64-elf-
```

The test makes one priority-3 task ready while another priority-3 task is
running, then briefly preempts the running task with a priority-5 task. Its
UART result is one of:

- `READY_PEER_FIRST`: the already-ready peer runs before the preempted task.
- `PREEMPTED_TASK_FIRST`: the interrupted task resumes first.
- `UNEXPECTED`: the observed trace did not match either expected ordering.

This experiment documents the current kernel behavior before any queue-policy
extension is proposed. A policy change should not be implemented until a real
workload shows that the alternative ordering improves a useful metric.

## Current multicore status

The normal images remain single-core FreeRTOS builds. A separate diagnostic
image now releases the three secondary Cortex-A72 cores:

```sh
make multicore-smoke CROSS=aarch64-elf-
```

Core 0 still owns FreeRTOS and all interrupts. Cores 1 through 3 run a small
bare-metal heartbeat loop on private stacks. The one-second UART report adds
`core1`, `core2`, and `core3`; all three counters must increase between reports.

This proves secondary-core entry, private stacks, shared-memory visibility, and
event wakeups. It does **not** prove FreeRTOS SMP or task affinity. Running
FreeRTOS tasks on multiple cores still requires a current SMP-capable kernel,
per-core exception state, per-core timer or tick handling, inter-processor
interrupts, scheduler locks, and cache-coherent atomic primitives. No multicore
scheduling result should be claimed until those pieces are integrated and
tested on hardware.

## Hardware run order

Use the same boot media and UART adapter for every image. Save the complete UART
capture for each step.

1. `threshold-semantics`
2. `ready-queue-test`
3. `multicore-smoke`
4. baseline benchmark for at least 70 seconds
5. priority raise and restore for at least 70 seconds
6. threshold for at least 70 seconds
7. scheduler suspension for at least 70 seconds

The 70-second runs provide ten warm-up reports and at least sixty retained
reports. Rotate the order of the four performance modes on later repetitions.
Do not publish or report measurements until each mode has at least five runs
and the raw captures are archived.


## Collecting results without a UART adapter

The Pi's normal Linux install can collect the results instead of a serial
adapter. Linux stays the default boot. Each image runs once through the
firmware's one-shot `tryboot`, copies everything it prints into a reserved
1 MiB block of RAM at `0x60000000`, and resets back to Linux through the
watchdog. Linux then reads the block over SSH. Nothing is written to the boot
filesystem while FreeRTOS runs.

Build the RAM-log variants (they also still print on the UART):

```sh
make benchmark-threshold CROSS=aarch64-elf- RAM_LOG=1   # 80 reports, then reset
make ram-persist-test CROSS=aarch64-elf-
```

`RAM_LOG_REPORTS=<n>` changes the run length. The reporter also kicks a
15-second hardware watchdog, so a hang returns the board to Linux and the log
is reported as incomplete.

From the Mac, with the Pi reachable over SSH (`PI=pi@<address>` to override):

```sh
tools/pi_run.sh setup     # once: reserve the RAM (dtoverlay=freertos-ramlog), reboot
tools/pi_run.sh persist   # must print ram_persistence=PASS before trusting logs
tools/pi_run.sh log FreeRTOS/Demo/CORTEX_A72_64-bit_Raspberrypi4/uart/ramlog-images/benchmark-threshold-ramlog.img results/threshold-1.log
```

The saved log has the same lines as a UART capture, so
`tools/analyze_benchmark.py` reads it unchanged. The persistence test checks
both directions: a nonce Linux writes before rebooting must reach the image,
and a 1 MiB pattern the image writes must reach Linux intact. If a power cycle
is ever needed, the board simply boots Linux.

Setup adds one `dtoverlay=freertos-ramlog` line to `config.txt` and installs
`freertos/kernel8.img` and `tryboot.txt` on the boot partition. Removing that
line undoes the reservation.
