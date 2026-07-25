/* user/user.ld — link the ring-3 program at the user-space virtual base.
 * The kernel's ELF loader maps PT_LOAD segments at these exact vaddrs with
 * PTE_USER, so the program runs unmodified at ring 3.
 *
 * v0.54: emit SEPARATE PT_LOAD segments so userland obeys W^X, the same policy
 * the kernel enforces on itself. Previously every section shared ONE PT_LOAD;
 * that segment was R+X only because init.c happened to contain no writable
 * data. The moment the v0.54 app library introduced static buffers (.bss), the
 * merged segment became RWE and the loader correctly rejected the image
 * ("reject: writable+executable segment"). Splitting text (R+X), rodata (R)
 * and data/bss (R+W) fixes that properly instead of weakening the check.     */
ENTRY(_start)
PHDRS
{
    text   PT_LOAD FLAGS(5);   /* R + X */
    rodata PT_LOAD FLAGS(4);   /* R     */
    data   PT_LOAD FLAGS(6);   /* R + W */
}
SECTIONS
{
    . = 0x500000000000;
    .text   : { *(.text .text.*) }                :text
    . = ALIGN(0x1000);
    .rodata : { *(.rodata .rodata.*) }            :rodata
    . = ALIGN(0x1000);
    .data   : { *(.data .data.*) }                :data
    .bss    : { *(.bss .bss.*) *(COMMON) }        :data

    /DISCARD/ : { *(.note.gnu.property) *(.eh_frame) *(.comment) }
}
