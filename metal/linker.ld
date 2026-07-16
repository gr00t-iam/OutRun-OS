/* OUTRUN OS — kernel linker script */
ENTRY(_start)

SECTIONS
{
    . = 1M;

    .boot : ALIGN(8) {
        KEEP(*(.multiboot_header))
    }

    .text : ALIGN(4K) {
        _stext = .;
        *(.text*)
        . = ALIGN(4K);
        _etext = .;
    }

    .rodata : ALIGN(4K) {
        *(.rodata*)
    }

    .data : ALIGN(4K) {
        *(.data*)
    }

    .bss : ALIGN(4K) {
        *(COMMON)
        *(.bss*)
    }
    . = ALIGN(4K);
    _kernel_end = .;
}
