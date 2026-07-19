; =============================================================================
; OUTRUN OS — RING-3 USER MODE (boot/usermode.asm)
; =============================================================================
; Everything needed to drop to ring 3 and trap back:
;   syscall_entry     — SYSCALL lands here (ring0). Swaps to a kernel stack,
;                       calls the C dispatcher, SYSRETs back to ring 3.
;   enter_user_mode   — builds an iretq frame and enters ring 3 for the first
;                       time, after saving a kernel resume point (legacy
;                       synchronous excursions from the boot thread).
;   enter_user_thread — same iretq entry but saves NOTHING: a first-class
;                       ring-3 scheduler thread never "returns" to the kernel;
;                       it leaves only via SYS_EXIT or a CPL3 fault, both of
;                       which reschedule.
;   resume_kernel     — longjmp back to kernel_main from SYS_EXIT (ring 0),
;                       legacy path only.
;   set_syscall_stack — C sets the per-syscall kernel stack top. Since v0.31
;                       the scheduler reloads this on every context switch, so
;                       each thread syscalls onto its OWN kernel stack.
;   user_blob         — a self-contained, position-independent ring-3 program
;                       that talks to the kernel ONLY through syscalls.
;
; v0.39: EVERY word of trap-path state below is PER-CPU, reached GS-relative
; through this core's `struct cpu_local` (GS_BASE points at it in both rings:
; ring 3 cannot change GS_BASE — no wrgsbase, CR4.FSGSBASE off — so no swapgs
; is needed anywhere). This is what lets several cores sit in ring 3 and take
; SYSCALLs *simultaneously*: no shared .bss scratch remains in the entry path.
;
; SYSCALL clobbers RCX (=return RIP) and R11 (=RFLAGS); RDI/RSI/RDX survive.
; Our ABI: RAX = syscall number, RDI = arg0, RSI = arg1.
; =============================================================================
bits 64

; struct cpu_local offsets (kernel64.c keeps these in lockstep)
CPUL_SYSCALL_RSP equ 8           ; this CPU's SYSCALL kernel stack top
CPUL_USER_RSP    equ 16          ; scratch: parked user RSP (2 insns, IF masked)
CPUL_KRSP        equ 24          ; kernel resume context (SYS_EXIT longjmp),
CPUL_KRBX        equ 32          ;   one per CPU so concurrent synchronous
CPUL_KRBP        equ 40          ;   ring-3 excursions on different cores
CPUL_KR12        equ 48          ;   unwind independently
CPUL_KR13        equ 56
CPUL_KR14        equ 64
CPUL_KR15        equ 72

global syscall_entry
global enter_user_mode
global enter_user_thread
global enter_user_resume
global resume_kernel
global set_syscall_stack
global user_blob_start
global user_blob_end
extern syscall_dispatch
extern dbg_syscall_exit              ; DEBUG_SYSCALL_EXIT: no-op unless armed (kernel64.c)

section .text

; ---- SYSCALL entry: RAX=num, RDI=a0, RSI=a1 --------------------------------
; GS-relative on every core: N cores can be in here at once, each on its own
; cpu_local and its own kernel stack. SFMASK keeps IF off across the two-insn
; scratch window, and no other core can touch %gs-addressed state but ours.
syscall_entry:
    mov [gs:CPUL_USER_RSP], rsp  ; SYSCALL does NOT switch RSP — park user RSP
    mov rsp, [gs:CPUL_SYSCALL_RSP] ; switch to THIS CPU's/thread's kernel stack
    push qword [gs:CPUL_USER_RSP] ; user RSP now lives on the thread's own stack:
                                 ; safe across a blocking syscall (yield/wait)
    ; The user-space ABI expects a syscall to clobber only RAX/RCX/R11. Our C
    ; dispatcher freely clobbers the caller-saved arg registers, so preserve
    ; them here.
    push rcx                     ; return RIP
    push r11                     ; user RFLAGS
    push rdi
    push rsi
    push rdx
    push r8
    push r9
    push r10
    sub rsp, 8                   ; 9 pushes above -> re-align to 16 for SysV call
    ; marshal user (RAX=num, RDI=a0, RSI=a1, RDX=a2) -> SysV(rdi,rsi,rdx,rcx)
    mov rcx, rdx                 ; a2 -> 4th C arg
    mov rdx, rsi                 ; a1 -> 3rd C arg
    mov rsi, rdi                 ; a0 -> 2nd C arg
    mov rdi, rax                 ; num -> 1st C arg
    call syscall_dispatch        ; return value in RAX (SYS_EXIT never returns)
    add rsp, 8
    ; DEBUG_SYSCALL_EXIT hook: fires for EVERY return value (success or error
    ; alike — this is the one shared epilogue), strictly before any saved
    ; register is popped back and before RSP/RIP reach SYSRET. Read the saved
    ; user RIP/RSP straight out of their pushed slots (offsets unchanged by
    ; the `add rsp,8` above), THEN push rax so the call's caller-saved
    ; clobbers (rax/rcx/rdx/rsi/rdi/r8-r11) touch nothing the pop sequence
    ; below still needs — those all still live in memory, untouched.
    mov rdi, [rsp+56]            ; saved user RIP (the pushed RCX slot)
    mov rsi, [rsp+64]            ; saved user RSP (the parked CPUL_USER_RSP)
    push rax                     ; preserve the syscall return value
    call dbg_syscall_exit        ; dbg_syscall_exit(rip, rsp); returns immediately unless armed
    pop rax
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rsi
    pop rdi
    pop r11
    pop rcx
    pop rsp                      ; user RSP, from THIS thread's kernel stack
    o64 sysret                   ; SYSRETQ: RIP<-RCX, RFLAGS<-R11, CS/SS<-user

; ---- uint64_t enter_user_mode(uint64_t entry /*rdi*/, uint64_t ustack /*rsi*/)
enter_user_mode:
    mov [gs:CPUL_KRBX], rbx      ; save callee-saved regs + RSP (PER CPU) so
    mov [gs:CPUL_KRBP], rbp      ; SYS_EXIT unwinds to THIS core's call site
    mov [gs:CPUL_KR12], r12
    mov [gs:CPUL_KR13], r13
    mov [gs:CPUL_KR14], r14
    mov [gs:CPUL_KR15], r15
    mov [gs:CPUL_KRSP], rsp
    mov ax, 0x23                 ; user data selector (0x20 | RPL3)
    mov ds, ax
    mov es, ax
    ; build the iretq frame: SS, RSP, RFLAGS, CS, RIP (pushed high->low)
    push 0x23                    ; SS  = user data
    push rsi                     ; RSP = user stack
    push 0x202                   ; RFLAGS: IF set (interrupts stay live in r3)
    push 0x2B                    ; CS  = user code64 (0x28 | RPL3)
    push rdi                     ; RIP = user entry
    iretq

; ---- void enter_user_thread(uint64_t entry /*rdi*/, uint64_t ustack /*rsi*/) -
; Ring-3 entry for a FIRST-CLASS scheduler thread. Deliberately saves no resume
; context: the thread's kernel half is its PCB + kernel stack, managed by the
; scheduler. SYS_EXIT and CPL3 faults reap the thread and reschedule; nothing
; ever "returns" here.
enter_user_thread:
    mov ax, 0x23                 ; user data selector (0x20 | RPL3)
    mov ds, ax
    mov es, ax
    push 0x23                    ; SS  = user data
    push rsi                     ; RSP = user stack
    push 0x202                   ; RFLAGS: IF set (the thread is preemptible)
    push 0x2B                    ; CS  = user code64 (0x28 | RPL3)
    push rdi                     ; RIP = user entry
    iretq

; ---- uint64_t enter_user_resume(struct uctx *u /*rdi*/) -------------------
; v0.39: rebuild a PREEMPTED ring-3 context — possibly on a DIFFERENT core
; than the one that captured it. Saves this core's kernel resume point exactly
; like enter_user_mode (so the next SYS_EXIT / preemption unwinds HERE), then
; restores every GPR plus RIP/RSP/RFLAGS from the capture and iretq's straight
; back into the middle of the interrupted user code.
; struct uctx offsets (mirrors isr_frame GPR order; kernel64.c is the master):
;   r15 0  r14 8  r13 16  r12 24  r11 32  r10 40  r9 48  r8 56
;   rbp 64 rdi 72 rsi 80  rdx 88  rcx 96  rbx 104 rax 112
;   rip 120  rsp 128  rflags 136
enter_user_resume:
    mov [gs:CPUL_KRBX], rbx
    mov [gs:CPUL_KRBP], rbp
    mov [gs:CPUL_KR12], r12
    mov [gs:CPUL_KR13], r13
    mov [gs:CPUL_KR14], r14
    mov [gs:CPUL_KR15], r15
    mov [gs:CPUL_KRSP], rsp
    mov ax, 0x23                 ; user data selectors (before rax is restored)
    mov ds, ax
    mov es, ax
    push qword 0x23              ; SS     = user data
    push qword [rdi+128]         ; RSP    = captured user stack pointer
    push qword [rdi+136]         ; RFLAGS = captured flags (IF was live)
    push qword 0x2B              ; CS     = user code64
    push qword [rdi+120]         ; RIP    = the interrupted instruction
    mov r15, [rdi+0]
    mov r14, [rdi+8]
    mov r13, [rdi+16]
    mov r12, [rdi+24]
    mov r11, [rdi+32]
    mov r10, [rdi+40]
    mov r9,  [rdi+48]
    mov r8,  [rdi+56]
    mov rbp, [rdi+64]
    mov rsi, [rdi+80]
    mov rdx, [rdi+88]
    mov rcx, [rdi+96]
    mov rbx, [rdi+104]
    mov rax, [rdi+112]
    mov rdi, [rdi+72]            ; rdi last — it was the pointer
    iretq

; ---- void resume_kernel(uint64_t retval /*rdi*/) — noreturn ---------------
resume_kernel:
    mov rax, rdi
    mov rsp, [gs:CPUL_KRSP]      ; restore THIS core's kernel resume context
    mov rbx, [gs:CPUL_KRBX]
    mov rbp, [gs:CPUL_KRBP]
    mov r12, [gs:CPUL_KR12]
    mov r13, [gs:CPUL_KR13]
    mov r14, [gs:CPUL_KR14]
    mov r15, [gs:CPUL_KR15]
    ret                          ; returns as if enter_user_mode() returned

; ---- void set_syscall_stack(uint64_t top /*rdi*/) -------------------------
; Writes the CALLING core's slot: the scheduler / exec wrappers always run on
; the CPU whose stack they are installing.
set_syscall_stack:
    mov [gs:CPUL_SYSCALL_RSP], rdi
    ret

; ===========================================================================
; USER PROGRAM (runs at ring 3). Position-independent: only RIP-relative data
; and the `syscall` instruction. Copied into a USER-mapped page by the kernel.
;   syscalls:  0=WRITE(cstr)  1=HW_PASSTHROUGH(handle)  2=EXIT(code)  3=WRITEHEX
; ===========================================================================
align 16
user_blob_start:
    ; SYS_WRITE(msg1)
    mov rax, 0
    lea rdi, [rel .msg1]
    syscall
    ; SYS_HW_PASSTHROUGH(handle=0xFFFF -> kernel's staged demo device)
    mov rax, 1
    mov rdi, 0xFFFF
    syscall
    push rax                     ; save result on the (USER) stack
    ; SYS_WRITE(msg2)
    mov rax, 0
    lea rdi, [rel .msg2]
    syscall
    ; SYS_WRITEHEX(result)
    pop rdi
    mov rax, 3
    syscall
    ; SYS_WRITE(newline)
    mov rax, 0
    lea rdi, [rel .nl]
    syscall
    ; SYS_EXIT(0)
    mov rax, 2
    xor rdi, rdi
    syscall
.hang:
    jmp .hang
.msg1: db "  [ring3 ] unprivileged process live; issuing SYSCALL trap...", 10, 0
.msg2: db "  [ring3 ] SYSCALL sys_hardware_passthrough returned ", 0
.nl:   db 10, 0
user_blob_end:
