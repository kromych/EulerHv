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
;    This module implements the AMD64-specific SVM VMRUN routine and the
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

    .code

    extern ShvSvmEntryHandler:proc
    extern ShvOsCaptureContext:proc

    ;
    ; VOID ShvSvmVmrun (VOID)
    ;
    ; Entered through ShvOsRestoreContext with the guest GPRs and XMM state, the
    ; guest VMCB address in RAX, and RSP at the CONTEXT on top of the hypervisor
    ; stack. The host VMCB address follows that CONTEXT, at RSP+4D0h
    ;
    ShvSvmVmrun PROC
    vmload  rax                 ; load the guest FS, GS, TR, LDTR and system
                                ; call MSRs
    vmrun   rax                 ; run the guest. #VMEXIT restores RSP and RAX
    vmsave  rax                 ; save the guest FS, GS, TR, LDTR and system
                                ; call MSRs
    mov     rax, [rsp+4D0h]     ; load the host ones, which #VMEXIT does not
    vmload  rax
    push    rcx                 ; save the RCX register, which we spill below
    lea     rcx, [rsp+8h]       ; store the context at the original RSP
    call    ShvOsCaptureContext ; save the guest GPRs and XMM state, as
                                ; ShvVmxEntry does
    pop     rax                 ; return the spilled RCX in the context
    mov     [rcx+80h], rax
    push    0                   ; a null return address, which ends stack
                                ; walks at the handler, as nothing called it
    jmp     ShvSvmEntryHandler  ; jump to the C code handler, with RSP aligned
                                ; as after a call
    ShvSvmVmrun ENDP

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

    ;
    ; UINT64 ShvSvmOwnHypercall (_In_ UINT64 Input,
    ;                            _In_ UINT64 InputPhysicalAddress,
    ;                            _In_ UINT64 OutputPhysicalAddress)
    ;
    ; Issues a hypercall of SimpleVisor's own to Hyper-V. The hypercall takes
    ; its input value in RCX and the parameter addresses in RDX and R8, and
    ; returns its result value in RAX, as the calling convention does
    ;
    ShvSvmOwnHypercall PROC
    vmmcall                     ; exit to Hyper-V from the host
    ret
    ShvSvmOwnHypercall ENDP

    end
