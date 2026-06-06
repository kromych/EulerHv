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
    ; there was none, such as after an NMI. The idle IDT must be loaded
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

    end
