;++
;
; Copyright (c) Alex Ionescu.  All rights reserved.
;
; Module:
;
;    shvosx64.asm
;
; Abstract:
;
;    This module implements AMD64-specific routines for the Simple Hyper Visor.
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

;
; The CONTEXT offsets below match the AMD64 CONTEXT in winnt.h
;
;   30h ContextFlags   34h MxCsr          38h SegCs ... 42h SegSs
;   44h EFlags         78h Rax ... 0F0h R15
;   98h Rsp            0F8h Rip           1A0h Xmm0 ... 290h Xmm15
;
; The CONTEXT must be 16 byte aligned for MOVAPS
;

CONTEXT_FULL equ 10000Bh

    .code

    _ltr PROC
    ltr     cx
    ret
    _ltr ENDP

    ShvOsCaptureContext PROC
    pushfq
    mov     [rcx+78h], rax
    mov     [rcx+80h], rcx
    mov     [rcx+88h], rdx
    mov     [rcx+0B8h], r8
    mov     [rcx+0C0h], r9
    mov     [rcx+0C8h], r10
    mov     [rcx+0D0h], r11

    mov     word ptr [rcx+38h], cs
    mov     word ptr [rcx+3Ah], ds
    mov     word ptr [rcx+3Ch], es
    mov     word ptr [rcx+42h], ss
    mov     word ptr [rcx+3Eh], fs
    mov     word ptr [rcx+40h], gs

    mov     [rcx+90h], rbx
    mov     [rcx+0A0h], rbp
    mov     [rcx+0A8h], rsi
    mov     [rcx+0B0h], rdi
    mov     [rcx+0D8h], r12
    mov     [rcx+0E0h], r13
    mov     [rcx+0E8h], r14
    mov     [rcx+0F0h], r15

    stmxcsr dword ptr [rcx+34h]
    movaps  [rcx+1A0h], xmm0
    movaps  [rcx+1B0h], xmm1
    movaps  [rcx+1C0h], xmm2
    movaps  [rcx+1D0h], xmm3
    movaps  [rcx+1E0h], xmm4
    movaps  [rcx+1F0h], xmm5
    movaps  [rcx+200h], xmm6
    movaps  [rcx+210h], xmm7
    movaps  [rcx+220h], xmm8
    movaps  [rcx+230h], xmm9
    movaps  [rcx+240h], xmm10
    movaps  [rcx+250h], xmm11
    movaps  [rcx+260h], xmm12
    movaps  [rcx+270h], xmm13
    movaps  [rcx+280h], xmm14
    movaps  [rcx+290h], xmm15
    mov     dword ptr [rcx+30h], CONTEXT_FULL

    lea     rax, [rsp+10h]
    mov     [rcx+98h], rax
    mov     rax, [rsp+8]
    mov     [rcx+0F8h], rax
    mov     eax, [rsp]
    mov     [rcx+44h], eax

    add     rsp, 8
    ret
    ShvOsCaptureContext ENDP

    ShvOsRestoreContext PROC
    mov     ax, [rcx+42h]
    mov     [rsp+20h], ax
    mov     rax, [rcx+98h]
    mov     [rsp+18h], rax
    mov     eax, [rcx+44h]
    mov     [rsp+10h], eax
    mov     ax, [rcx+38h]
    mov     [rsp+8], ax
    mov     rax, [rcx+0F8h]
    mov     [rsp], rax

    movaps  xmm0, [rcx+1A0h]
    movaps  xmm1, [rcx+1B0h]
    movaps  xmm2, [rcx+1C0h]
    movaps  xmm3, [rcx+1D0h]
    movaps  xmm4, [rcx+1E0h]
    movaps  xmm5, [rcx+1F0h]
    movaps  xmm6, [rcx+200h]
    movaps  xmm7, [rcx+210h]
    movaps  xmm8, [rcx+220h]
    movaps  xmm9, [rcx+230h]
    movaps  xmm10, [rcx+240h]
    movaps  xmm11, [rcx+250h]
    movaps  xmm12, [rcx+260h]
    movaps  xmm13, [rcx+270h]
    movaps  xmm14, [rcx+280h]
    movaps  xmm15, [rcx+290h]
    ldmxcsr dword ptr [rcx+34h]

    mov     rax, [rcx+78h]
    mov     rdx, [rcx+88h]
    mov     r8, [rcx+0B8h]
    mov     r9, [rcx+0C0h]
    mov     r10, [rcx+0C8h]
    mov     r11, [rcx+0D0h]
    cli

    mov     rbx, [rcx+90h]
    mov     rsi, [rcx+0A8h]
    mov     rdi, [rcx+0B0h]
    mov     rbp, [rcx+0A0h]
    mov     r12, [rcx+0D8h]
    mov     r13, [rcx+0E0h]
    mov     r14, [rcx+0E8h]
    mov     r15, [rcx+0F0h]
    mov     rcx, [rcx+80h]

    iretq
    ShvOsRestoreContext ENDP

    end
