ENTRY(_boot)

/* The Pi 4 firmware loads a kernel with an arm64 Image header at 0x200000 and
 * ignores kernel_address=0x80000 (measured 9 Nov 2026 with a probe that
 * recorded its entry PC). Everything up to PT_BASE + PT_SIZE stays under
 * 10 MiB, which the interference image maps as its image region. */
CODE_BASE   = 0x00200000;
CODE_SIZE   = 0x00200000;
DATA_BASE   = CODE_BASE + CODE_SIZE;
DATA_SIZE   = 0x00400000;
STACK_TOP   = DATA_BASE + DATA_SIZE; /* STACK in the upper end of DATA */
PT_BASE     = DATA_BASE + DATA_SIZE;
PT_SIZE     = 0x00200000;
__image_size = PT_BASE + PT_SIZE - CODE_BASE; /* header image_size, incl. BSS/stack/PT */

MEMORY {
    CODE  (rx) : ORIGIN = CODE_BASE, LENGTH = CODE_SIZE
    DATA  (rw) : ORIGIN = DATA_BASE, LENGTH = DATA_SIZE
    PT    (rw) : ORIGIN = PT_BASE,   LENGTH = PT_SIZE
}
 
SECTIONS
{
    /* arm64 Image header first (see startup.S), then the 2 KiB-aligned vectors. */
    .text :
    {
        __start = .;
        __text_start = .;
        KEEP(*(.text.head))
        KEEP(*(.text.boot))  /* entry code straight after the header, as in the
                                minimal probe that boots; see startup.S */
        KEEP(*(.vectors))
        *(.text)
        __text_end = .;
    } > CODE
 
    .data :
    {
        . = ALIGN(4096); /* align to page size */
        __data_start = DATA_BASE;
        *(.data)
        __data_end = .;
    } > DATA
 
    .bss :
    {
        . = ALIGN(4096); /* align to page size */
        __bss_start__ = .;
        __bss_start = .;
        bss = .;
        *(.bss)
        __bss_end__ = .;
        __bss_end = .;
    } > DATA

    .heap :
    {
        . = ALIGN(4096); /* align to page size */
        __heap_start__ = .;
        end = __heap_start__;
        _end = end;
        __end = end;
        __end__ = end;
        KEEP(*(.heap))
        __heap_end__ = .;
        __HeapLimit = __heap_end__;
    } > DATA

    .stack :
    {
    } > DATA
    stack_top = STACK_TOP;

    /* NOLOAD: set_pt() fills every entry, so the zeros need not be in
     * kernel8.img (they would make it about 6 MiB). */
    .pt (NOLOAD) :
    {
        . = ALIGN(4096); /* align to page size */
        *(.pt)
    } > PT

}
__bss_size = (__bss_end - __bss_start)>>3;
