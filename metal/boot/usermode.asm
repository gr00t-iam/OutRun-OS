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
; SYSCALL clobbers RCX (=return RIP) and R11 (=RFLAGS); RDI/RSI/RDX survive.
; Our ABI: RAX = syscall number, RDI = arg0, RSI = arg1.
; =============================================================================
bits 64

global syscall_entry
global enter_user_mode
global enter_user_thread
global resume_kernel
global set_syscall_stack
global user_blob_start
global user_blob_end
extern syscall_dispatch

section .bss
align 16
g_ksrsp: resq 1          ; kernel syscall stack top (per-thread, reloaded on switch)
g_ursp:  resq 1          ; SCRATCH only: user RSP parks here for two instructions
                         ; (IF is masked by SFMASK, uniprocessor) before moving
                         ; onto the thread's own kernel stack, so a syscall that
                         ; blocks can no longer have its user RSP clobbered by
                         ; another thread's syscall
; kernel resume context (for SYS_EXIT longjmp)
g_krsp:  resq 1
g_krbx:  resq 1
g_krbp:  resq 1
g_kr12:  resq 1
g_kr13:  resq 1
g_kr14:  resq 1
g_kr15:  resq 1

section .text

; ---- SYSCALL entry: RAX=num, RDI=a0, RSI=a1 --------------------------------
syscall_entry:
    mov [g_ursp], rsp            ; SYSCALL does NOT switch RSP — park user RSP
    mov rsp, [g_ksrsp]           ; switch to THIS THREAD's kernel syscall stack
    push qword [g_ursp]          ; user RSP now lives on the thread's own stack:
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

; ---- void enter_user_mode(uint64_t entry /*rdi*/, uint64_t ustack /*rsi*/) --
enter_user_mode:
    mov [g_krbx], rbx            ; save callee-saved regs + RSP so SYS_EXIT can
    mov [g_krbp], rbp            ; return us straight back into kernel_main
    mov [g_kr12], r12
    mov [g_kr13], r13
    mov [g_kr14], r14
    mov [g_kr15], r15
    mov [g_krsp], rsp
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

; ---- void resume_kernel(uint64_t retval /*rdi*/) — noreturn ---------------
resume_kernel:
    mov rax, rdi
    mov rsp, [g_krsp]            ; restore the kernel stack captured above
    mov rbx, [g_krbx]
    mov rbp, [g_krbp]
    mov r12, [g_kr12]
    mov r13, [g_kr13]
    mov r14, [g_kr14]
    mov r15, [g_kr15]
    ret                          ; returns as if enter_user_mode() returned

; ---- void set_syscall_stack(uint64_t top /*rdi*/) -------------------------
set_syscall_stack:
    mov [g_ksrsp], rdi
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
