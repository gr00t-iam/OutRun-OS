; =============================================================================
; OUTRUN OS — INTERRUPT STUBS (boot/isr.asm)
; =============================================================================
; 48 vectors: CPU exceptions 0-31 and remapped hardware IRQs 32-47.
; Every stub normalizes the stack to (vector, error_code, saved regs) and
; calls the C dispatcher. In long mode the CPU 16-byte-aligns RSP before
; pushing the 5-quad interrupt frame; our push counts keep the SysV ABI
; alignment contract intact at the call site.
; =============================================================================
bits 64
extern isr_dispatch
global idt_load
global ap_tramp_start
global ap_tramp_end

; v0.35: the 16->64-bit AP boot trampoline, carried as data and copied to
; physical 0x8000 by smp_init before the INIT-SIPI-SIPI sequence.
section .rodata
ap_tramp_start: incbin "build/apboot.bin"
ap_tramp_end:

%macro ISR_NOERR 1
global isr%1
isr%1:
    push qword 0                     ; fake error code
    push qword %1                    ; vector
    jmp  isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push qword %1                    ; CPU already pushed the error code
    jmp  isr_common
%endmacro

section .text

; CPU exceptions. 8, 10-14, 17, 21 push an error code; the rest do not.
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

; Hardware IRQs (PIC remapped to 32-47)
%assign v 32
%rep 16
ISR_NOERR v
%assign v v+1
%endrep

; v0.35: inter-processor interrupts (LAPIC fixed vectors)
ISR_NOERR 48                         ; IPI: ping / wake
ISR_NOERR 49                         ; IPI: TLB shootdown

isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp                     ; arg0: pointer to saved frame
    call isr_dispatch

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16                      ; drop vector + error code
    iretq

; void idt_load(void *idtr)
idt_load:
    lidt [rdi]
    ret
