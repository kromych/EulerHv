;++
;
; Copyright (c) Alex Ionescu.  All rights reserved.
;
; Module:
;
;    shvvmxhvx64.asm
;
; Abstract:
;
;    This module implements the AMD64-specific SimpleVisor VMENTRY routine.
;
; Author:
;
;    Alex Ionescu (@aionescu) 16-Mar-2016 - Initial version
;
; Environment:
;
;    Kernel mode only.
;
;--

    .code

    extern ShvVmxEntryHandler:proc
    extern ShvOsCaptureContext:proc

    ShvVmxEntry PROC
    push    rcx                 ; save the RCX register, which we spill below
    lea     rcx, [rsp+8h]       ; store the context in the stack, bias for
                                ; the return address and the push we just did.
    call    ShvOsCaptureContext ; save the current register state.
                                ; note that this is a specially written function
                                ; which has the following key characteristics:
                                ;   1) it does not taint the value of RCX
                                ;   2) it does not spill any registers, nor
                                ;      expect home space to be allocated for it
    jmp     ShvVmxEntryHandler  ; jump to the C code handler. the push above
                                ; keeps RSP ABI-aligned without a return
                                ; address, and the handler's home space
                                ; overlaps the unused P1Home-P4Home fields of
                                ; the context, so this is safe in any build
    ShvVmxEntry ENDP

    ;
    ; VOID ShvVmxHypercall (_Inout_ PCONTEXT Context)
    ;
    ; Forwards a guest hypercall to Hyper-V. Its inputs come from the guest's
    ; RCX, RDX, R8 and XMM0-XMM5, and the result goes to its RAX, with fast
    ; hypercall outputs in RDX, R8 and XMM0-XMM5. The CONTEXT offsets match
    ; winnt.h, and the CONTEXT must be 16 byte aligned for MOVAPS
    ;
    ShvVmxHypercall PROC
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
    vmcall                      ; exit to Hyper-V from root mode
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
    ShvVmxHypercall ENDP

    end
