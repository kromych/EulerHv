;++
;
; Copyright (c) Roman Kisel 2026.  All rights reserved.
;
; Module:
;
;    shvmsrx64.asm
;
; Abstract:
;
;    This module implements the AMD64-specific routines with which SimpleVisor
;    accesses the MSRs that exit from root mode, for VT-x and SVM alike.
;
; Author:
;
;    kromych (vdso@mailbox.org) 29-Oct-2026 - Initial version
;
; Environment:
;
;    Hypervisor mode only.
;
;--

    .code

    ;
    ; UINT8 ShvMsrRead (_In_ UINT32 Msr, _Out_ UINT64* Value)
    ; UINT8 ShvMsrWrite (_In_ UINT32 Msr, _In_ UINT64 Value)
    ;
    ; Read or write an MSR, and return TRUE, or FALSE if the RDMSR or WRMSR
    ; raised #GP, as the processor or the hypervisor SimpleVisor is nested
    ; under does for an MSR it does not have. The loaded IDT must have
    ; ShvMsrFault as its #GP gate
    ;
    ShvMsrRead PROC
    mov     r8, rdx             ; RDMSR returns the value in EDX:EAX
ShvMsrReadInstruction::
    rdmsr
    shl     rdx, 32
    or      rax, rdx
    mov     [r8], rax
    mov     eax, 1
    ret
    ShvMsrRead ENDP

    ShvMsrWrite PROC
    mov     eax, edx            ; WRMSR takes the value in EDX:EAX
    shr     rdx, 32
ShvMsrWriteInstruction::
    wrmsr
    mov     eax, 1
    ret
    ShvMsrWrite ENDP

    ;
    ; The #GP gate of the idle IDT. A #GP from the RDMSR or WRMSR above
    ; resumes at ShvMsrFailed, which returns FALSE in their place, as neither
    ; uses the stack. Nothing else raises #GP while the idle IDT is loaded,
    ; so anything else is unrecoverable
    ;
    ShvMsrFault PROC
    push    rax
    lea     rax, ShvMsrReadInstruction
    cmp     rax, [rsp+10h]      ; the faulting RIP, past RAX and the error code
    je      Failed
    lea     rax, ShvMsrWriteInstruction
    cmp     rax, [rsp+10h]
    je      Failed
    int     3                   ; break into the debugger and stop
Halt:
    hlt
    jmp     Halt
Failed:
    lea     rax, ShvMsrFailed
    mov     [rsp+10h], rax      ; resume at ShvMsrFailed
    pop     rax
    add     rsp, 8              ; drop the error code
    iretq
    ShvMsrFault ENDP

    ShvMsrFailed PROC
    xor     eax, eax
    ret
    ShvMsrFailed ENDP

    end
