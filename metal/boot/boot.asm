; =============================================================================
; OUTRUN OS — BARE-METAL BOOTSTRAP (boot/boot.asm)
; =============================================================================
; GRUB (Multiboot2) drops us here in 32-bit protected mode. This file:
;   1. verifies the CPU supports x86_64 long mode
;   2. builds identity page tables for the first 1 GiB (2 MiB huge pages)
;   3. enables PAE + long mode + paging
;   4. loads a 64-bit GDT and far-jumps into long mode
;   5. hands control (and the Multiboot2 info pointer) to the C kernel
; =============================================================================

; ---- Multiboot2 header -------------------------------------------------------
section .multiboot_header
align 8
header_start:
    dd 0xe85250d6                                   ; multiboot2 magic
    dd 0                                            ; arch: i386 protected mode
    dd header_end - header_start                    ; header length
    dd 0x100000000 - (0xe85250d6 + 0 + (header_end - header_start)) ; checksum

    ; framebuffer request tag: ask GRUB for a linear framebuffer graphics mode
    align 8
    dw 5                                            ; type = framebuffer
    dw 0                                            ; flags
    dd 20                                           ; size
    dd 1024                                         ; requested width
    dd 768                                          ; requested height
    dd 32                                           ; requested depth (bpp)

    ; end tag
    align 8
    dw 0
    dw 0
    dd 8
header_end:

; ---- 32-bit entry ------------------------------------------------------------
section .text
bits 32
global _start
extern kernel_main

_start:
    mov esp, stack_top
    mov [multiboot_ptr], ebx        ; GRUB passes info struct in EBX

    call check_cpuid
    call check_long_mode
    call setup_page_tables
    call enable_paging

    lgdt [gdt64.pointer]
    jmp  gdt64.code_seg:long_mode_start

; ---- CPU feature checks --------------------------------------------------------
check_cpuid:                         ; can we flip EFLAGS.ID at all?
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    cmp eax, ecx
    je .fail
    ret
.fail:
    mov al, '1'
    jmp boot_error

check_long_mode:
    mov eax, 0x80000000              ; extended CPUID available?
    cpuid
    cmp eax, 0x80000001
    jb .fail
    mov eax, 0x80000001              ; LM bit?
    cpuid
    test edx, 1 << 29
    jz .fail
    ret
.fail:
    mov al, '2'
    jmp boot_error

boot_error:                          ; print "ERR: <code>" via VGA and halt
    mov dword [0xb8000], 0x4f524f45
    mov dword [0xb8004], 0x4f3a4f52
    mov dword [0xb8008], 0x4f204f20
    mov byte  [0xb800a], al
    hlt
    jmp $

; ---- Paging: identity-map first 1 GiB with 2 MiB pages -------------------------
setup_page_tables:
    mov eax, pdpt
    or  eax, 0b11                    ; present | writable
    mov [pml4], eax

    mov eax, pd
    or  eax, 0b11
    mov [pdpt], eax

    xor ecx, ecx
.map_pd:
    mov eax, 0x200000                ; 2 MiB
    mul ecx
    or  eax, 0b10000011              ; present | writable | huge
    mov [pd + ecx * 8], eax
    inc ecx
    cmp ecx, 512
    jne .map_pd
    ret

enable_paging:
    mov eax, pml4                    ; CR3 -> PML4
    mov cr3, eax

    mov eax, cr4                     ; enable PAE
    or  eax, 1 << 5
    mov cr4, eax

    mov ecx, 0xC0000080              ; EFER: enable long mode + no-execute
    rdmsr
    or  eax, 1 << 8                  ; EFER.LME (long mode enable)
    or  eax, 1 << 11                 ; EFER.NXE (no-execute enable)
    wrmsr

    mov eax, cr0                     ; enable paging
    or  eax, 1 << 31
    mov cr0, eax
    ret

; ---- 64-bit entry ---------------------------------------------------------------
bits 64
long_mode_start:
    mov ax, gdt64.data_seg
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov rsp, stack_top
    mov edi, [multiboot_ptr]         ; arg0: multiboot2 info (zero-extended)
    call kernel_main

.halt:
    cli
    hlt
    jmp .halt

; ---- Data -----------------------------------------------------------------------
section .bss
align 4096
pml4:   resb 4096
pdpt:   resb 4096
pd:     resb 4096
stack_bottom:
        resb 64 * 1024               ; 64 KiB kernel stack
stack_top:
multiboot_ptr: resd 1

section .rodata
gdt64:
    dq 0                                             ; null
.code_seg: equ $ - gdt64
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53)         ; 64-bit code
.data_seg: equ $ - gdt64
    dq (1<<41) | (1<<44) | (1<<47)                   ; data
.pointer:
    dw $ - gdt64 - 1
    dq gdt64
