;++
;
; Copyright (c) Roman Kisel 2026.  All rights reserved.
;
; Module:
;
;    shvidlex64.asm
;
; Abstract:
;
;    This module implements the AMD64-specific routines SimpleVisor idles in
;    when the guest halts, for VT-x and SVM alike.
;
; Author:
;
;    kromych (vdso@mailbox.org) 17-Oct-2026 - Initial version
;
; Environment:
;
;    Hypervisor mode only.
;
;--

    .code

    ;
    ; UINT32 ShvIdleWaitForInterrupt (VOID)
    ;
    ; Halts with interrupts enabled until something wakes the processor up,
    ; and returns the vector of the interrupt the idle IDT took, or 0 if
    ; there was none, such as after an exception the OS's gate handled. The
    ; idle IDT must be loaded
    ;
    ShvIdleWaitForInterrupt PROC
    xor     eax, eax            ; no interrupt taken yet
    sti                         ; the STI shadow keeps interrupts out until
    hlt                         ; the HLT, so none is missed before halting
    cli                         ; the stubs already cleared IF, but the OS's
    ret                         ; NMI and exception handlers restore it
    ShvIdleWaitForInterrupt ENDP

    ;
    ; The idle IDT's interrupt stubs, SHV_IDLE_STUB_SIZE bytes apart. Each
    ; returns its vector in EAX to ShvIdleWaitForInterrupt, and clears IF in
    ; the interrupt frame's RFLAGS so that no other interrupt replaces it. The
    ; interrupt stays in service in the local APIC until the guest's handler
    ; signals its EOI
    ;
    ALIGN 16
    ShvIdleStubs PROC
    vector = 32
    REPT 256 - 32
    mov     eax, vector
    and     byte ptr [rsp+11h], NOT 2   ; RFLAGS.IF is bit 9 of [rsp+10h]
    iretq
    ALIGN 16
    vector = vector + 1
    ENDM
    ShvIdleStubs ENDP

    ;
    ; The idle IDT's NMI gate. Running the OS's NMI handler in root mode would
    ; run its code on the hypervisor stack, so take SimpleVisor's flush IPIs
    ; here, with ShvVpHandleRootNmi, saving the volatile registers it may use,
    ; which passes the guest's other NMIs on to it. An NMI that arrives
    ; elsewhere breaks into the debugger and stops this processor, with the
    ; interrupt frame and the registers on the stack for the debugger
    ;
    ; The processor aligns the stack before pushing the 5 qwords of the frame,
    ; so the 7 pushes below leave it aligned for the XMM saves and the call
    ;
    EXTERN ShvVpHandleRootNmi:PROC

    ShvIdleNmiStub PROC
    push    rax
    push    rcx
    push    rdx
    push    r8
    push    r9
    push    r10
    push    r11
    sub     rsp, 80h            ; home space, and XMM0-XMM5 above it
    movdqa  [rsp+20h], xmm0
    movdqa  [rsp+30h], xmm1
    movdqa  [rsp+40h], xmm2
    movdqa  [rsp+50h], xmm3
    movdqa  [rsp+60h], xmm4
    movdqa  [rsp+70h], xmm5
    cld
    mov     rcx, rsp            ; any address on this stack finds the VP data
    call    ShvVpHandleRootNmi
    test    al, al
    jz      ShvIdleNmiStubUnexpected
    movdqa  xmm0, [rsp+20h]
    movdqa  xmm1, [rsp+30h]
    movdqa  xmm2, [rsp+40h]
    movdqa  xmm3, [rsp+50h]
    movdqa  xmm4, [rsp+60h]
    movdqa  xmm5, [rsp+70h]
    add     rsp, 80h
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     rdx
    pop     rcx
    pop     rax
    iretq
    ShvIdleNmiStubUnexpected:
    int     3
    ShvIdleNmiStubHalt:
    hlt
    jmp     ShvIdleNmiStubHalt
    ShvIdleNmiStub ENDP

    end
