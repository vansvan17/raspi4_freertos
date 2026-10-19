# Research status

## Question

Does bounded preemption provide a measurable or analyzable advantage over
existing FreeRTOS mechanisms for a low-jitter control workload, and how do
ready-queue placement and multicore interference affect that result?

## Completed in software

- Optional single-core preemption-threshold prototype.
- Identical control workload for normal fixed-priority scheduling, temporary
  priority raise and restore, preemption threshold, and scheduler suspension.
- Hardware-counter instrumentation for controller latency and urgent-task
  lateness, plus context-switch, interference, and deadline counters.
- UART log parser and CSV summarizer.
- Deterministic nested-preemption semantic test.
- Deterministic equal-priority ready-queue placement experiment.
- Secondary-core smoke image with private stacks and shared heartbeat counters.
- Successful cross-compilation of all four benchmark modes and all three
  diagnostic images with `aarch64-elf-gcc`.

## Not completed and not claimed

- No measured Raspberry Pi results have been collected yet.
- The secondary cores do not run FreeRTOS tasks.
- Task affinity, inter-processor interrupts, and FreeRTOS SMP are not present.
- No evidence yet shows that preemption thresholds outperform a temporary
  priority change.
- No FreeRTOS kernel pull request should be opened until the comparison and
  semantics are supported by hardware evidence.

## Hardware acceptance criteria

1. The threshold semantic image prints `PASS` on three cold boots.
2. The queue-order image reports the same ordering on three cold boots.
3. All three secondary-core counters increase for at least five minutes.
4. Each scheduling mode produces five clean runs of at least seventy seconds.
5. Each run uses the same firmware, clock settings, workload constants, and
   ambient setup, with mode order rotated across repetitions.
6. Raw UART logs, build commit, configuration, and run metadata are archived.
7. The analyzer accepts every log without mixed-mode or incomplete-sample
   errors.

## Next engineering milestone

After the single-core evidence is complete, migrate the experiment to the
current FreeRTOS kernel and implement the Raspberry Pi 4 platform hooks needed
by an SMP-capable AArch64 port. The first SMP milestone is not performance. It
is two pinned tasks running on different cores with reliable cross-core wakeup
and no scheduler corruption under a long stress test.
