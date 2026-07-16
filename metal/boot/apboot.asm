; =============================================================================
; OUTRUN OS — AP BOOT TRAMPOLINE (boot/apboot.asm)  [v0.35 SMP groundwork]
; =============================================================================
; Assembled FLAT BINARY at org 0x8000. The BSP copies this blob to physical
; 0x8000 and sends INIT-SIPI-SIPI with vector 0x08, so each application
; processor starts here in 16-bit real mode and climbs to 64-bit long mode:
;   real mode -> lgdt (private mini-GDT) -> protected mode -> PAE + kernel
;   CR3 + EFER.LME|NXE -> paging on -> 64-bit far jump -> load stack/arg from
;   the mailbox -> jump into the kernel's ap_entry64.
; The mailbox lives in the NEXT page (phys 0x9000), which stays RW+NX under
; the kernel's W^X regime — the trampoline only READS it, and NX permits data
; reads. THIS page (0x8000) is remapped R+X (never writable) by smp_init after
; the copy: the climb executes under paging without a W+X page ever existing.
; (First cut kept the mailbox in this page and left it NX — the instruction
; fetch after CR0.PG faulted at 0x8050 and triple-faulted the AP. The BSP's
; own W^X hardening was doing its job.)
;   0x9000 = kernel CR3 (phys, <4G)      0x9008 = 64-bit kernel entry point
;   0x9010 = this AP's kernel stack top  0x9018 = this AP's cpu index (rdi)
; The private GDT only carries the transition far jumps; the kernel entry
; immediately reloads the REAL kernel GDT (whose 0x08 is the 64-bit code
; selector every IDT gate names) before enabling interrupts.
; =============================================================================
[bits 16]
org 0x8000

ap_start:
    cli
    xor ax, ax
    mov ds, ax
    lgdt [gdt_ptr]
    mov eax, cr0
    or  eax, 1                       ; CR0.PE
    mov cr0, eax
    jmp dword 0x08:pm32              ; flush prefetch, load 32-bit CS

[bits 32]
pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, cr4
    or  eax, 1 << 5                  ; CR4.PAE
    mov cr4, eax
    mov eax, [0x9000]                ; kernel PML4 (the boot page tables)
    mov cr3, eax
    mov ecx, 0xC0000080              ; EFER: long mode + NX, same as the BSP
    rdmsr
    or  eax, (1 << 8) | (1 << 11)    ; LME | NXE
    wrmsr
    mov eax, cr0
    or  eax, 0x80010000              ; PG | WP (W^X discipline holds on APs too)
    mov cr0, eax
    jmp 0x18:lm64                    ; 64-bit CS in the private GDT

[bits 64]
lm64:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov rsp, [0x9010]                ; this AP's own kernel stack
    mov rdi, [0x9018]                ; cpu index -> first C argument
    mov rax, [0x9008]                ; ap_entry64 (noreturn)
    jmp rax

align 16
gdt:
    dq 0
    dq 0x00CF9A000000FFFF            ; 0x08 code32 (transition only)
    dq 0x00CF92000000FFFF            ; 0x10 data
    dq 0x00AF9A000000FFFF            ; 0x18 code64 (transition only)
gdt_ptr:
    dw gdt_ptr - gdt - 1
    dd gdt

