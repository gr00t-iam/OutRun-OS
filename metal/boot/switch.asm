; =============================================================================
; OUTRUN OS — boot/switch.asm : cooperative kernel-thread context switch
; =============================================================================
; void switch_context(uint64_t *save_rsp /*rdi*/, uint64_t new_rsp /*rsi*/)
;   - saves the current thread's callee-saved registers + RFLAGS on its stack
;   - stores the resulting RSP into *save_rsp
;   - loads the next thread's RSP and restores its saved state
;   - `ret` resumes the next thread wherever it last called switch_context
;     (or, for a brand-new thread, at its prepared trampoline).
; RFLAGS is saved/restored so the interrupt-enable state travels with the thread.
; =============================================================================
bits 64
global switch_context

switch_context:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    pushfq
    mov  [rdi], rsp          ; *save_rsp = current RSP
    mov  rsp, rsi            ; switch to the next thread's stack
    popfq
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  rbp
    pop  rbx
    ret
