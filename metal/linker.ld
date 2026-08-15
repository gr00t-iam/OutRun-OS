/* OUTRUN OS — kernel linker script */
ENTRY(_start)

/* v0.78: THREE load segments instead of one.
 *
 * Until now every section — .boot, .text, .rodata, .eh_frame, .data, .bss —
 * shared a single PT_LOAD, so the program header had to be the union of their
 * permissions: RWE. That was the last warning in the build
 * ("has a LOAD segment with RWX permissions"), and it was a true statement
 * about the image even though the RUNNING kernel is hardened: harden_kernel_wx()
 * remaps [_stext,_etext) R+X and everything else RW+NX before any of it matters.
 *
 * Splitting the segments is what lets the static image say what the runtime
 * already enforces. It also exposed something the single segment was hiding:
 * harden_kernel_wx() derived "read-only" from _etext alone, so .rodata and
 * .eh_frame — which sit ABOVE _etext — were being mapped RW+NX. Read-only data
 * was writable at runtime. The _srodata/_erodata bounds below exist to close
 * that, and they are the reason this is not merely a cosmetic change.
 *
 * FLAGS are the ELF p_flags bits: 1=X, 2=W, 4=R. So 5 = R+X, 4 = R, 6 = R+W.
 * GRUB loads multiboot2 kernels per program header, so several PT_LOADs are
 * loaded exactly as one was; the header itself stays in the first segment and
 * therefore still lands within multiboot2's search window. */
PHDRS
{
    text   PT_LOAD FLAGS(5);   /* R + X : .boot, .text                        */
    rodata PT_LOAD FLAGS(4);   /* R     : .rodata, .eh_frame                  */
    data   PT_LOAD FLAGS(6);   /* R + W : .data, .bss                         */
}

SECTIONS
{
    . = 1M;

    .boot : ALIGN(8) {
        KEEP(*(.multiboot_header))
    } :text

    .text : ALIGN(4K) {
        _stext = .;
        *(.text*)
        . = ALIGN(4K);
        _etext = .;
    } :text

    /* Read-only data gets its own segment AND its own symbols. .eh_frame is
     * named explicitly rather than left to fall where it may: it was previously
     * placed by default between .rodata and .data, which is how it ended up on
     * the writable side of the only boundary the kernel knew about. */
    . = ALIGN(4K);
    _srodata = .;
    .rodata : ALIGN(4K) {
        *(.rodata*)
    } :rodata
    .eh_frame : ALIGN(8) {
        *(.eh_frame*)
    } :rodata
    . = ALIGN(4K);
    _erodata = .;

    .data : ALIGN(4K) {
        _sdata = .;
        *(.data*)
    } :data

    .bss : ALIGN(4K) {
        *(COMMON)
        *(.bss*)
    } :data
    . = ALIGN(4K);
    _edata = .;
    _kernel_end = .;
}
