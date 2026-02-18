/*++

Copyright (c) Alex Ionescu.  All rights reserved.

Module Name:

    shvvmxhv.c

Abstract:

    This module implements the Simple Hyper Visor itself.

Author:

    Alex Ionescu (@aionescu) 16-Mar-2016 - Initial version

Environment:

    Hypervisor mode only, IRQL MAX_IRQL

--*/

#include "shv.h"

DECLSPEC_NORETURN
VOID
ShvVmxResume (
    VOID
    )
{
    //
    // Issue a VMXRESUME. The reason that we've defined an entire function for
    // this sole instruction is both so that we can use it as the target of the
    // VMCS when re-entering the VM After a VM-Exit, as well as so that we can
    // decorate it with the DECLSPEC_NORETURN marker, which is not set on the
    // intrinsic (as it can fail in case of an error).
    //
    __vmx_vmresume();
}

uintptr_t
FORCEINLINE
ShvVmxRead (
    _In_ UINT32 VmcsFieldId
    )
{
    size_t FieldData;

    //
    // Because VMXREAD returns an error code, and not the data, it is painful
    // to use in most circumstances. This simple function simplifies it use.
    //
    __vmx_vmread(VmcsFieldId, &FieldData);
    return FieldData;
}

INT32
ShvVmxLaunch (
    VOID
    )
{
    INT32 failureCode;

    //
    // Launch the VMCS
    //
    __vmx_vmlaunch();

    //
    // If we got here, either VMCS setup failed in some way, or the launch
    // did not proceed as planned.
    //
    failureCode = (INT32)ShvVmxRead(VM_INSTRUCTION_ERROR);
    __vmx_off();

    //
    // Return the error back to the caller
    //
    return failureCode;
}

VOID
ShvVmxHandleInvd (
    VOID
    )
{
    //
    // This is the handler for the INVD instruction. Technically it may be more
    // correct to use __invd instead of __wbinvd, but that intrinsic doesn't
    // actually exist. Additionally, the Windows kernel (or HAL) don't contain
    // any example of INVD actually ever being used. Finally, Hyper-V itself
    // handles INVD by issuing WBINVD as well, so we'll just do that here too.
    //
    __wbinvd();
}

DECLSPEC_NORETURN
VOID
ShvVmxHalt (
    VOID
    )
{
    //
    // Unrecoverable state. Break into the debugger and stop this processor
    //
    __debugbreak();
    for (;;)
    {
        __halt();
    }
}

DECLSPEC_NORETURN
VOID
ShvVmxHandleFatalExit (
    _In_ PSHV_VP_STATE VpState
    )
{
    PSHV_VP_DATA vpData = VpState->VpData;

    //
    // An exit SimpleVisor cannot handle, such as an EPT violation outside the
    // identity map. Its instruction length is undefined so the guest cannot be
    // resumed. Save the details for the debugger and halt
    //
    vpData->FatalExitReason = VpState->ExitReason;
    vpData->FatalExitQualification = ShvVmxRead(EXIT_QUALIFICATION);
    vpData->FatalGuestPhysicalAddress = ShvVmxRead(GUEST_PHYSICAL_ADDRESS);
    vpData->FatalGuestLinearAddress = ShvVmxRead(GUEST_LINEAR_ADDRESS);
    vpData->FatalGuestRip = VpState->GuestRip;
    ShvVmxHalt();
}

VOID
ShvVmxInjectGeneralProtection (
    _In_ PSHV_VP_STATE VpState
    )
{
    //
    // Inject #GP(0). RIP stays at the faulting instruction
    //
    __vmx_vmwrite(VM_ENTRY_INTR_INFO, INTR_INFO_VALID |
                                      INTR_INFO_DELIVER_CODE |
                                      INTR_INFO_TYPE_HARD_EXCEPTION |
                                      INTR_INFO_VECTOR_GP);
    __vmx_vmwrite(VM_ENTRY_EXCEPTION_ERROR_CODE, 0);
    VpState->SkipRipAdvance = TRUE;
}

VOID
ShvVmxHandleCpuid (
    _In_ PSHV_VP_STATE VpState
    )
{
    INT32 cpu_info[4];

    //
    // Check for the magic CPUID sequence, and check that it is coming from
    // Ring 0. Technically we could also check the RIP and see if this falls
    // in the expected function, but we may want to allow a separate "unload"
    // driver or code at some point.
    //
    if ((VpState->VpRegs->Rax == 0x41414141) &&
        (VpState->VpRegs->Rcx == 0x42424242) &&
        ((ShvVmxRead(GUEST_CS_SELECTOR) & RPL_MASK) == DPL_SYSTEM))
    {
        VpState->ExitVm = TRUE;
        return;
    }

    //
    // Otherwise, issue the CPUID to the logical processor based on the indexes
    // on the VP's GPRs.
    //
    __cpuidex(cpu_info, (INT32)VpState->VpRegs->Rax, (INT32)VpState->VpRegs->Rcx);

    //
    // Check if this was CPUID 1h, which is the features request.
    //
    if (VpState->VpRegs->Rax == 1)
    {
        //
        // Set the Hypervisor Present-bit in RCX, which Intel and AMD have both
        // reserved for this indication.
        //
        cpu_info[2] |= HYPERV_HYPERVISOR_PRESENT_BIT;
    }
    else if (((UINT32)VpState->VpRegs->Rax == 7) &&
             ((UINT32)VpState->VpRegs->Rcx == 0) &&
             (VpState->VpData->UserWaitPauseEnabled == FALSE))
    {
        //
        // Hide WAITPKG (ECX bit 5) without the enable user wait and pause
        // control, as UMONITOR, UMWAIT and TPAUSE would raise #UD
        //
        cpu_info[2] &= ~(1 << 5);
    }
    else if ((VpState->VpRegs->Rax == HYPERV_CPUID_VENDOR_AND_MAX_FUNCTIONS) &&
             (VpState->VpData->UnderlyingHypervisor == FALSE))
    {
        //
        // Return our highest hypervisor leaf and the SimpleVisor vendor, which
        // guests probe once they see the Hypervisor Present-bit
        //
        // When nested pass the underlying hypervisor's through, so the guest
        // still knows the platform
        //
        cpu_info[0] = HYPERV_CPUID_INTERFACE;
        cpu_info[1] = 'pmiS';
        cpu_info[2] = 'iVel';
        cpu_info[3] = 'ros';
    }
    else if (VpState->VpRegs->Rax == HYPERV_CPUID_INTERFACE)
    {
        //
        // Return our interface identifier
        //
        cpu_info[0] = ' vhS';
    }

    //
    // Copy the values from the logical processor registers into the VP GPRs.
    //
    VpState->VpRegs->Rax = cpu_info[0];
    VpState->VpRegs->Rbx = cpu_info[1];
    VpState->VpRegs->Rcx = cpu_info[2];
    VpState->VpRegs->Rdx = cpu_info[3];
}

VOID
ShvVmxHandleXsetbv (
    _In_ PSHV_VP_STATE VpState
    )
{
    INT32 cpu_info[4];
    UINT64 xcr0;
    UINT64 supported;

    //
    // An invalid value raises #GP in root mode and crashes the host. Check it
    // as the processor does and inject #GP instead. The CPL and CR4.OSXSAVE
    // checks happen before the exit
    //
    xcr0 = VpState->VpRegs->Rdx << 32 | (UINT32)VpState->VpRegs->Rax;
    __cpuidex(cpu_info, 0xD, 0);
    supported = (UINT64)(UINT32)cpu_info[3] << 32 | (UINT32)cpu_info[0];

    if (((UINT32)VpState->VpRegs->Rcx != 0) ||              // Only XCR0 exists
        ((xcr0 & ~supported) != 0) ||                       // Unsupported state
        ((xcr0 & XCR0_X87) == 0) ||                         // x87 must be set
        (((xcr0 & XCR0_AVX) != 0) &&
         ((xcr0 & XCR0_SSE) == 0)) ||                       // AVX requires SSE
        (((xcr0 & XCR0_BNDREG) != 0) !=
         ((xcr0 & XCR0_BNDCSR) != 0)) ||                    // MPX all or none
        (((xcr0 & XCR0_AVX512) != 0) &&
         ((xcr0 & (XCR0_AVX512 | XCR0_AVX | XCR0_SSE)) !=
          (XCR0_AVX512 | XCR0_AVX | XCR0_SSE))) ||          // AVX-512 all or none
        (((xcr0 & XCR0_TILECFG) != 0) !=
         ((xcr0 & XCR0_TILEDATA) != 0)))                    // AMX all or none
    {
        ShvVmxInjectGeneralProtection(VpState);
        return;
    }

    //
    // Issue the XSETBV instruction on the native logical processor
    //
    _xsetbv((UINT32)VpState->VpRegs->Rcx, xcr0);
}

VOID
ShvVmxHandleMsr (
    _In_ PSHV_VP_STATE VpState
    )
{
    UINT32 msr;
    UINT64 value;

    //
    // Only MSRs outside the MSR bitmap ranges exit. None exist on bare metal
    // so raise #GP. Under Hyper-V pass its synthetic MSRs through from root
    // mode, which exits to Hyper-V. A #GP from Hyper-V would be fatal, but a
    // guest only uses the synthetic MSRs Hyper-V advertised
    //
    msr = (UINT32)VpState->VpRegs->Rcx;
    if ((VpState->VpData->UnderlyingHypervisorIsHyperV == FALSE) ||
        (msr < HYPERV_SYNTHETIC_MSR_MIN) ||
        (msr > HYPERV_SYNTHETIC_MSR_MAX))
    {
        ShvVmxInjectGeneralProtection(VpState);
        return;
    }

    if (VpState->ExitReason == EXIT_REASON_MSR_READ)
    {
        //
        // RDMSR returns the value in EDX:EAX, clearing the upper halves
        //
        value = __readmsr(msr);
        VpState->VpRegs->Rax = (UINT32)value;
        VpState->VpRegs->Rdx = value >> 32;
    }
    else
    {
        //
        // WRMSR takes the value from EDX:EAX, ignoring the upper halves
        //
        value = VpState->VpRegs->Rdx << 32 | (UINT32)VpState->VpRegs->Rax;
        __writemsr(msr, value);
    }
}

VOID
ShvVmxHandleVmx (
    _In_ PSHV_VP_STATE VpState
    )
{
    //
    // Report VMfailInvalid. CF is set and PF, AF, ZF, SF and OF are cleared
    //
    VpState->GuestEFlags &= ~(EFLAGS_CARRY_FLAG | EFLAGS_PARITY_FLAG |
                              EFLAGS_AUX_CARRY_FLAG | EFLAGS_ZERO_FLAG |
                              EFLAGS_SIGN_FLAG | EFLAGS_OVERFLOW_FLAG);
    VpState->GuestEFlags |= EFLAGS_CARRY_FLAG;

    //
    // RFLAGs is actually restored from the VMCS, so update it here
    //
    __vmx_vmwrite(GUEST_RFLAGS, VpState->GuestEFlags);
}

VOID
ShvVmxHandleExit (
    _In_ PSHV_VP_STATE VpState
    )
{
    //
    // This is the generic VM-Exit handler. Decode the reason for the exit and
    // call the appropriate handler. As per Intel specifications, given that we
    // have requested no optional exits whatsoever, we should only see CPUID,
    // INVD, XSETBV and other VMX instructions. GETSEC cannot happen as we do
    // not run in SMX context.
    //
    // RDMSR and WRMSR exit for MSRs outside the MSR bitmap. Hyper-V's
    // synthetic MSRs pass through when nested under it, others raise #GP
    //
    // EPT violations and misconfigurations and any other exit are fatal
    //
    switch (VpState->ExitReason)
    {
    case EXIT_REASON_CPUID:
        ShvVmxHandleCpuid(VpState);
        break;
    case EXIT_REASON_INVD:
        ShvVmxHandleInvd();
        break;
    case EXIT_REASON_XSETBV:
        ShvVmxHandleXsetbv(VpState);
        break;
    case EXIT_REASON_MSR_READ:
    case EXIT_REASON_MSR_WRITE:
        ShvVmxHandleMsr(VpState);
        break;
    case EXIT_REASON_VMCALL:
    case EXIT_REASON_VMCLEAR:
    case EXIT_REASON_VMLAUNCH:
    case EXIT_REASON_VMPTRLD:
    case EXIT_REASON_VMPTRST:
    case EXIT_REASON_VMREAD:
    case EXIT_REASON_VMRESUME:
    case EXIT_REASON_VMWRITE:
    case EXIT_REASON_VMXOFF:
    case EXIT_REASON_VMXON:
        ShvVmxHandleVmx(VpState);
        break;
    case EXIT_REASON_EPT_VIOLATION:
    case EXIT_REASON_EPT_MISCONFIG:
    default:
        ShvVmxHandleFatalExit(VpState);
    }

    //
    // Move the instruction pointer past the exiting instruction, unless the
    // handler injected a fault
    //
    if (VpState->SkipRipAdvance == FALSE)
    {
        VpState->GuestRip += ShvVmxRead(VM_EXIT_INSTRUCTION_LEN);
        __vmx_vmwrite(GUEST_RIP, VpState->GuestRip);
    }
}

DECLSPEC_NORETURN
VOID
ShvVmxEntryHandler (
    _In_ PCONTEXT Context
    )
{
    SHV_VP_STATE guestContext;
    PSHV_VP_DATA vpData;

    //
    // Because we had to use RCX when calling ShvOsCaptureContext, its value
    // was actually pushed on the stack right before the call. Go dig into the
    // stack to find it, and overwrite the bogus value that's there now.
    //
    Context->Rcx = *(UINT64*)((uintptr_t)Context - sizeof(Context->Rcx));

    //
    // Get the per-VP data for this processor.
    //
    vpData = (VOID*)((uintptr_t)(Context + 1) - KERNEL_STACK_SIZE);

    //
    // The VP data sits at the bottom of the hypervisor stack. A clobbered
    // canary means the stack overflowed into it, so halt
    //
    if (vpData->StackCanary != SHV_STACK_CANARY)
    {
        ShvVmxHalt();
    }

    //
    // Build a little stack context to make it easier to keep track of certain
    // guest state, such as the RIP/RSP/RFLAGS, and the exit reason. The rest
    // of the general purpose registers come from the context structure that we
    // captured on our own with RtlCaptureContext in the assembly entrypoint.
    //
    guestContext.GuestEFlags = ShvVmxRead(GUEST_RFLAGS);
    guestContext.GuestRip = ShvVmxRead(GUEST_RIP);
    guestContext.GuestRsp = ShvVmxRead(GUEST_RSP);
    guestContext.ExitReason = ShvVmxRead(VM_EXIT_REASON) & 0xFFFF;
    guestContext.VpRegs = Context;
    guestContext.VpData = vpData;
    guestContext.ExitVm = FALSE;
    guestContext.SkipRipAdvance = FALSE;

    //
    // Call the generic handler
    //
    ShvVmxHandleExit(&guestContext);

    //
    // Did we hit the magic exit sequence, or should we resume back to the VM
    // context?
    //
    if (guestContext.ExitVm != FALSE)
    {
        //
        // Return the VP Data structure in RAX:RBX which is going to be part of
        // the CPUID response that the caller (ShvVpUninitialize) expects back.
        // Return confirmation in RCX that we are loaded
        //
        Context->Rax = (uintptr_t)vpData >> 32;
        Context->Rbx = (uintptr_t)vpData & 0xFFFFFFFF;
        Context->Rcx = 0x43434343;

        //
        // Perform any OS-specific CPU uninitialization work
        //
        ShvOsUnprepareProcessor(vpData);

        //
        // Our callback routine may have interrupted an arbitrary user process,
        // and therefore not a thread running with a systemwide page directory.
        // Therefore if we return back to the original caller after turning off
        // VMX, it will keep our current "host" CR3 value which we set on entry
        // to the PML4 of the SYSTEM process. We want to return back with the
        // correct value of the "guest" CR3, so that the currently executing
        // process continues to run with its expected address space mappings.
        //
        __writecr3(ShvVmxRead(GUEST_CR3));

        //
        // Finally, restore the stack, instruction pointer and EFLAGS to the
        // original values present when the instruction causing our VM-Exit
        // execute (such as ShvVpUninitialize). This will effectively act as
        // a longjmp back to that location.
        //
        Context->Rsp = guestContext.GuestRsp;
        Context->Rip = (UINT64)guestContext.GuestRip;
        Context->EFlags = (UINT32)guestContext.GuestEFlags;

        //
        // Turn off VMX root mode on this logical processor. We're done here.
        //
        __vmx_off();
    }
    else
    {
        //
        // Because we won't be returning back into assembly code, nothing will
        // ever do the "pop rcx" that must technically be done. Leave RSP as is
        // so the extra 8 bytes stand in for a return address and keep
        // ShvVmxResume ABI-aligned. HOST_RSP is reloaded on every VM-Exit, so
        // the stack does not drift.
        //
        // Then, return into a VMXRESUME intrinsic, which we broke out as its
        // own function, in order to allow this to work. No assembly code will
        // be needed as ShvOsRestoreContext will fix all the GPRs and RSP.
        //
        Context->Rip = (UINT64)ShvVmxResume;
    }

    //
    // Restore the context to either ShvVmxResume, in which case the CPU's VMX
    // facility will do the "true" return back to the VM (but without restoring
    // GPRs, which is why we must do it here), or to the original guest's RIP,
    // which we use in case an exit was requested. In this case VMX must now be
    // off, and this will look like a longjmp to the original stack and RIP.
    //
    ShvOsRestoreContext(Context);
}

