/* user/user.ld — link the ring-3 program at the user-space virtual base.
 * The kernel's ELF loader maps PT_LOAD segments at these exact vaddrs with
 * PTE_USER, so the program runs unmodified at ring 3.                        */
ENTRY(_start)
SECTIONS
{
    . = 0x500000000000;
    .text   : { *(.text .text.*) }
    . = ALIGN(0x1000);
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(0x1000);
    .data   : { *(.data .data.*) }
    .bss    : { *(.bss .bss.*) *(COMMON) }
}
