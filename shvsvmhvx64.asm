;++
;
; Copyright (c) Roman Kisel 2026.  All rights reserved.
;
; Module:
;
;    shvsvmhvx64.asm
;
; Abstract:
;
;    This module implements the AMD64-specific SVM VMRUN loop and the
;    hypercall forwarding.
;
; Author:
;
;    kromych (vdso@mailbox.org) 23-Jun-2026 - Initial version
;
; Environment:
;
;    Kernel mode only.
;
;--

    include ksamd64.inc

    extern ShvSvmEntryHandler:proc

    ;
    ; The guest GPRs in a CONTEXT, with the host values that they clobber
    ; above it. The frame keeps RSP aligned for the call
    ;
    CONTEXT_SIZE    equ     4D0h
    HOST_VPDATA     equ     4D0h
    HOST_VMCBPA     equ     4D8h
    HOST_R14        equ     4E0h
    FRAME_SIZE      equ     4F0h

    .code

    ;
    ; VOID ShvSvmRun (
    ;     _In_ PSHV_VP_DATA  VpData,          ; RCX
    ;     _In_ UINT64         VmcbPa,          ; RDX
    ;     _In_ PCONTEXT       InitialContext    ; R8
    ;     )
    ;
    ; Runs the guest until VMRUN fails. On unload ShvSvmEntryHandler returns
    ; to the guest instead
    ;
    ShvSvmRun PROC
    push    rbp
    push    rbx
    push    rdi
    push    rsi
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, FRAME_SIZE

    mov     r12, rcx                ; VpData
    mov     r13, rdx                ; VmcbPa

    ;
    ; Copy the initial guest GPRs but RAX, which is in the VMCB
    ;
    mov     rax, CxRcx[r8]
    mov     CxRcx[rsp], rax
    mov     rax, CxRdx[r8]
    mov     CxRdx[rsp], rax
    mov     rax, CxRbx[r8]
    mov     CxRbx[rsp], rax
    mov     rax, CxRbp[r8]
    mov     CxRbp[rsp], rax
    mov     rax, CxRsi[r8]
    mov     CxRsi[rsp], rax
    mov     rax, CxRdi[r8]
    mov     CxRdi[rsp], rax
    mov     rax, CxR8[r8]
    mov     CxR8[rsp], rax
    mov     rax, CxR9[r8]
    mov     CxR9[rsp], rax
    mov     rax, CxR10[r8]
    mov     CxR10[rsp], rax
    mov     rax, CxR11[r8]
    mov     CxR11[rsp], rax
    mov     rax, CxR12[r8]
    mov     CxR12[rsp], rax
    mov     rax, CxR13[r8]
    mov     CxR13[rsp], rax
    mov     rax, CxR14[r8]
    mov     CxR14[rsp], rax
    mov     rax, CxR15[r8]
    mov     CxR15[rsp], rax

SvmLoop:
    ;
    ; Save the host pointers that the guest GPR loads will clobber
    ;
    mov     [rsp+HOST_VPDATA], r12
    mov     [rsp+HOST_VMCBPA], r13

    ;
    ; Load the guest FS, GS, TR, LDTR and system call MSRs
    ;
    mov     rax, r13
    vmload  rax

    ;
    ; Load the guest GPRs. VMRUN loads RAX from the VMCB
    ;
    mov     rcx, CxRcx[rsp]
    mov     rdx, CxRdx[rsp]
    mov     rbx, CxRbx[rsp]
    mov     rbp, CxRbp[rsp]
    mov     rsi, CxRsi[rsp]
    mov     rdi, CxRdi[rsp]
    mov     r8,  CxR8[rsp]
    mov     r9,  CxR9[rsp]
    mov     r10, CxR10[rsp]
    mov     r11, CxR11[rsp]
    mov     r12, CxR12[rsp]
    mov     r13, CxR13[rsp]
    mov     r14, CxR14[rsp]
    mov     r15, CxR15[rsp]

    ;
    ; Run the guest. #VMEXIT saves RAX to the VMCB, and leaves the other
    ; guest GPRs in place
    ;
    mov     rax, [rsp+HOST_VMCBPA]
    vmrun   rax

    ;
    ; Save the guest GPRs
    ;
    mov     CxRcx[rsp], rcx
    mov     CxRdx[rsp], rdx
    mov     CxRbx[rsp], rbx
    mov     CxRbp[rsp], rbp
    mov     CxRsi[rsp], rsi
    mov     CxRdi[rsp], rdi
    mov     CxR8[rsp],  r8
    mov     CxR9[rsp],  r9
    mov     CxR10[rsp], r10
    mov     CxR11[rsp], r11
    mov     CxR12[rsp], r12
    mov     CxR13[rsp], r13
    mov     CxR14[rsp], r14
    mov     CxR15[rsp], r15

    ;
    ; Restore the host pointers
    ;
    mov     r12, [rsp+HOST_VPDATA]
    mov     r13, [rsp+HOST_VMCBPA]

    ;
    ; Save the guest FS, GS, TR, LDTR and system call MSRs
    ;
    mov     rax, r13
    vmsave  rax

    ;
    ; Handle the exit
    ;
    lea     rcx, [rsp]              ; guest GPRs
    mov     rdx, r12                ; VpData
    sub     rsp, 20h
    call    ShvSvmEntryHandler
    add     rsp, 20h

    ;
    ; Resume the guest, unless VMRUN failed
    ;
    test    al, al
    jz      SvmLoop

    ;
    ; Return to ShvSvmLaunch
    ;
    add     rsp, FRAME_SIZE
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rsi
    pop     rdi
    pop     rbx
    pop     rbp
    ret
    ShvSvmRun ENDP

    ;
    ; VOID ShvSvmHypercall (_Inout_ PCONTEXT Context)
    ;
    ; Forwards a guest hypercall to Hyper-V. Its inputs come from the guest's
    ; RCX, RDX, R8 and XMM0-XMM5, and the result goes to its RAX, with fast
    ; hypercall outputs in RDX, R8 and XMM0-XMM5. The CONTEXT offsets match
    ; winnt.h, and the CONTEXT must be 16 byte aligned for MOVAPS
    ;
    ShvSvmHypercall PROC
    movaps  xmm0, [rcx+1A0h]    ; load the fast hypercall XMM inputs
    movaps  xmm1, [rcx+1B0h]
    movaps  xmm2, [rcx+1C0h]
    movaps  xmm3, [rcx+1D0h]
    movaps  xmm4, [rcx+1E0h]
    movaps  xmm5, [rcx+1F0h]
    mov     rdx, [rcx+88h]      ; input parameters GPA, or fast input
    mov     r8, [rcx+0B8h]      ; output parameters GPA, or fast input
    push    rcx                 ; save the context pointer
    mov     rcx, [rcx+80h]      ; hypercall input value
    vmmcall                     ; exit to Hyper-V from root mode
    xchg    rcx, [rsp]          ; restore the context pointer
    pop     qword ptr [rcx+80h] ; return the (rep start index updated) RCX
    mov     [rcx+78h], rax      ; return the hypercall result value
    mov     [rcx+88h], rdx      ; return any fast hypercall outputs
    mov     [rcx+0B8h], r8
    movaps  [rcx+1A0h], xmm0
    movaps  [rcx+1B0h], xmm1
    movaps  [rcx+1C0h], xmm2
    movaps  [rcx+1D0h], xmm3
    movaps  [rcx+1E0h], xmm4
    movaps  [rcx+1F0h], xmm5
    ret
    ShvSvmHypercall ENDP

    end
