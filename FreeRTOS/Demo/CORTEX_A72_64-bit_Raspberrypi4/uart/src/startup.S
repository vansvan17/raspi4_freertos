// arm64 Linux Image header, so the firmware treats this as a 64-bit kernel and
// loads it at 0x200000 (text_offset 0), where raspberrypi4.ld links it.
// Same header as a minimal probe that booted and returned on 9 Nov 2026.
.section ".text.head", "ax"
_image_head:
    b       _boot                       // code0
    .word   0                           // code1
    .quad   0                           // text_offset
    .quad   __image_size                // image_size
    .quad   0xA                         // flags: LE, 4K pages, any 2 MiB base
    .quad   0, 0, 0                     // res2-res4
    .ascii  "ARM\x64"                  // magic
    .word   0                           // res5

.section ".text.boot"
 
// Make _start global.
.globl _boot
 
_boot:
    .if BOOT_BREADCRUMBS
    // Diagnostic builds only. Arm the PM watchdog (15 s) at the very first
    // instruction, so a hang anywhere after entry still resets back to Linux,
    // then leave breadcrumbs in reserved RAM (0x300FF100) that Linux reads
    // afterwards: [0] stage, [1] CurrentEL, [2] CNTFRQ, [3] blinks, [4] MPIDR.
    // The firmware clears the tryboot flag itself, so a bare reset is enough.
    mov     x10, #0xFE100000            // PM base
    mov     w9, #0x5A0F0000             // PASSWORD | 15 s (15 << 16)
    str     w9, [x10, #0x24]            // PM_WDOG
    ldr     w9, [x10, #0x1C]            // PM_RSTC
    and     w9, w9, #0xFFFFFFCF
    mov     w11, #0x0020
    movk    w11, #0x5A00, lsl #16       // PASSWORD | WRCFG_FULL_RESET
    orr     w9, w9, w11
    str     w9, [x10, #0x1C]
    dsb     sy
    mov     x12, #0x300F0000
    movk    x12, #0xF100                // x12 = 0x300FF100
    mov     w9, #0x0001
    movk    w9, #0xBC00, lsl #16
    str     w9, [x12]                   // stage 1: entered _boot
    mrs     x9, CurrentEL
    str     w9, [x12, #4]
    mrs     x9, cntfrq_el0
    str     w9, [x12, #8]
    mrs     x9, mpidr_el1
    str     w9, [x12, #16]
    dsb     sy
    .if BOOT_BREADCRUMBS == 2
    // Entry-only test: boot partition 0, reset in 10 ticks, stop here.
    ldr     w9, [x10, #0x20]            // PM_RSTS
    mov     w8, #0xFAAA
    movk    w8, #0x00FF, lsl #16        // 0x00FFFAAA = low 24 bits minus 0x555
    and     w9, w9, w8
    mov     w8, #0x5A000000
    orr     w9, w9, w8
    str     w9, [x10, #0x20]
    mov     w9, #0x000A
    movk    w9, #0x5A00, lsl #16
    str     w9, [x10, #0x24]            // PM_WDOG = 10 ticks
    dsb     sy
1:  wfe
    b       1b
    .endif
    .endif
    // Boot directly on core #0. A diagnostic build can release cores 1-3.
    mrs     x1, mpidr_el1
    and     x1, x1, #3
    cbz     x1, start_el2

    .if MULTICORE_SMOKE_TEST
    b       secondary_wait
    .else
    b       park_core
    .endif

start_el2:
    bl      el2_open_counters
    // enable AArch64 in EL1.
    mov     x0, #(1 << 31)      // AArch64
    orr     x0, x0, #(1 << 1)   // SWIO hardwired on Pi4
    msr     hcr_el2, x0
    isb
    // change execution level to EL1.
    mov     x2, #0x3c5         // D=1, A=1, I=1, F=1 M=EL1h
    msr     spsr_el2, x2
    isb
    adr     x2, start_el1
    msr     elr_el2, x2
    isb
    eret

start_el1:
    .if BOOT_BREADCRUMBS
    mov     x12, #0x300F0000
    movk    x12, #0xF100
    mov     w9, #0x0002
    movk    w9, #0xBC00, lsl #16
    str     w9, [x12]                   // stage 2: reached EL1
    dsb     sy
    .endif
    // enable FPU
    mrs     x1, cpacr_el1
    mov     x0, #(3 << 20)
    orr     x0, x1, x0
    msr     cpacr_el1, x0
    // set sp
    adrp    x1, stack_top
    mov     sp, x1
    // MMU is intentionally left disabled for this standalone image.
    // clear bss
    ldr     x1, =__bss_start
    ldr     w2, =__bss_size
1:  cbz     w2, 2f
    str     xzr, [x1], #8
    sub     w2, w2, #1
    cbnz    w2, 1b

2:
    .if BOOT_BREADCRUMBS
    mov     x12, #0x300F0000
    movk    x12, #0xF100
    mov     w9, #0x0003
    movk    w9, #0xBC00, lsl #16
    str     w9, [x12]                   // stage 3: BSS cleared, calling main
    dsb     sy
    .endif
    bl      main
    ret

secondary_wait:
    // Released cores arrive in EL2 like core 0. Drop them to EL1 so they can
    // share core 0's EL1 page tables and use their own PMU.
    bl      el2_open_counters
    mov     x0, #(1 << 31)      // AArch64
    orr     x0, x0, #(1 << 1)   // SWIO hardwired on Pi4
    msr     hcr_el2, x0
    mov     x2, #0x3c5          // D=1, A=1, I=1, F=1 M=EL1h
    msr     spsr_el2, x2
    adr     x2, secondary_el1
    msr     elr_el2, x2
    isb
    eret

secondary_el1:
    mrs     x1, mpidr_el1
    and     x1, x1, #3
    mrs     x0, cpacr_el1
    orr     x0, x0, #(3 << 20)  // enable FPU
    msr     cpacr_el1, x0
    isb
    // Give every secondary core a private 16 KiB stack.
    adrp    x2, stack_top
    mov     x3, #0x4000
    mul     x3, x1, x3
    sub     sp, x2, x3

    // Core 0 clears BSS before publishing the release flag.
    ldr     x2, =secondary_core_release
3:  ldr     w3, [x2]
    cbnz    w3, secondary_start
    wfe
    b       3b

secondary_start:
    mov     x0, x1
    bl      secondary_core_main
    b       park_core

park_core:
    wfe
    b       park_core

// Lets EL1 read the physical counter and use every PMU counter without
// trapping to EL2. Clobbers x0.
el2_open_counters:
    mrs     x0, cnthctl_el2
    orr     x0, x0, #3          // EL1PCEN, EL1PCTEN
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr
    mrs     x0, pmcr_el0
    ubfx    x0, x0, #11, #5     // HPMN = PMCR_EL0.N, no PMU traps
    msr     mdcr_el2, x0
    isb
    ret
