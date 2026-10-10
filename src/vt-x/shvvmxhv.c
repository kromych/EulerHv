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

#include "shvvmx.h"

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

VOID
ShvVmxOff (
    _In_ PSHV_VP_DATA VpData
    )
{
    //
    // Leave VMX operation, and clear CR4.VMXE if SimpleVisor set it. Other
    // hypervisors take VMXE for VMX being in use, and do not load
    //
    __vmx_off();
    if (VpData->VmxSetVmxe != FALSE)
    {
        __writecr4(__readcr4() & ~CR4_VMXE);
    }
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
    _In_ PSHV_VP_DATA VpData
    )
{
    INT32 failureCode;
    UINT64 capabilities;
    UINT64 descriptor[2];

    //
    // VMXON and VMXOFF keep cached mappings, and VPID 1 and the EPTP may be
    // left over from an earlier load or another hypervisor. Invalidate them
    // so that the guest does not start on stale translations
    //
    capabilities = VpData->MsrData[MSR_IA32_VMX_EPT_VPID_CAP - MSR_IA32_VMX_BASIC].QuadPart;
    if (VpData->EptControls != 0)
    {
        descriptor[0] = ShvVmxRead(EPT_POINTER);
        descriptor[1] = 0;
        if ((capabilities & (VMX_EPT_INVEPT_BIT | VMX_EPT_EXTENT_CONTEXT_BIT)) ==
            (VMX_EPT_INVEPT_BIT | VMX_EPT_EXTENT_CONTEXT_BIT))
        {
            ShvVmxInvept(VMX_INVEPT_SINGLE_CONTEXT, descriptor);
        }
        else if ((capabilities & (VMX_EPT_INVEPT_BIT | VMX_EPT_EXTENT_GLOBAL_BIT)) ==
                 (VMX_EPT_INVEPT_BIT | VMX_EPT_EXTENT_GLOBAL_BIT))
        {
            ShvVmxInvept(VMX_INVEPT_ALL_CONTEXT, descriptor);
        }

        descriptor[0] = ShvVmxRead(VIRTUAL_PROCESSOR_ID);
        descriptor[1] = 0;
        if ((capabilities & (VMX_VPID_INVVPID_BIT | VMX_VPID_EXTENT_SINGLE_CONTEXT_BIT)) ==
            (VMX_VPID_INVVPID_BIT | VMX_VPID_EXTENT_SINGLE_CONTEXT_BIT))
        {
            ShvVmxInvvpid(VMX_INVVPID_SINGLE_CONTEXT, descriptor);
        }
        else if ((capabilities & (VMX_VPID_INVVPID_BIT | VMX_VPID_EXTENT_ALL_CONTEXT_BIT)) ==
                 (VMX_VPID_INVVPID_BIT | VMX_VPID_EXTENT_ALL_CONTEXT_BIT))
        {
            ShvVmxInvvpid(VMX_INVVPID_ALL_CONTEXT, descriptor);
        }
    }

    //
    // Launch the VMCS, based on the guest data that was loaded into the
    // various VMCS fields by ShvVmxSetupVmcsForVp. This will cause the
    // processor to jump to ShvVpRestoreAfterLaunch on success, or return
    // back to the caller on failure.
    //
    __vmx_vmlaunch();

    //
    // If we got here, either VMCS setup failed in some way, or the launch
    // did not proceed as planned.
    //
    failureCode = (INT32)ShvVmxRead(VM_INSTRUCTION_ERROR);
    ShvVmxOff(VpData);

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
    vpData->FatalGuestRsp = VpState->GuestRsp;

    //
    // Print them along with the guest state that VM entry checks, which the
    // debugger cannot see. After a failed VM entry this is the refused state
    //
    ShvOsDebugPrint("SimpleVisor: fatal VM-Exit on CPU %d, VP data 0x%llX\n",
                    vpData->ProcessorIndex, (UINT64)vpData);
    ShvOsDebugPrint("  Exit reason 0x%llX, qualification 0x%llX, GPA 0x%llX, GLA 0x%llX\n",
                    (UINT64)ShvVmxRead(VM_EXIT_REASON), vpData->FatalExitQualification,
                    vpData->FatalGuestPhysicalAddress, vpData->FatalGuestLinearAddress);
    ShvOsDebugPrint("  RIP 0x%llX, RSP 0x%llX, RFLAGS 0x%llX, CR0 0x%llX, CR4 0x%llX\n",
                    VpState->GuestRip, VpState->GuestRsp, VpState->GuestEFlags,
                    (UINT64)ShvVmxRead(GUEST_CR0), (UINT64)ShvVmxRead(GUEST_CR4));
    ShvOsDebugPrint("  CS 0x%llX (AR 0x%llX), SS 0x%llX (AR 0x%llX)\n",
                    (UINT64)ShvVmxRead(GUEST_CS_SELECTOR), (UINT64)ShvVmxRead(GUEST_CS_AR_BYTES),
                    (UINT64)ShvVmxRead(GUEST_SS_SELECTOR), (UINT64)ShvVmxRead(GUEST_SS_AR_BYTES));
    ShvOsDebugPrint("  Interruptibility 0x%llX, activity state %llu, pending debug exceptions 0x%llX\n",
                    (UINT64)ShvVmxRead(GUEST_INTERRUPTIBILITY_INFO), (UINT64)ShvVmxRead(GUEST_ACTIVITY_STATE),
                    (UINT64)ShvVmxRead(GUEST_PENDING_DBG_EXCEPTIONS));
    ShvOsDebugPrint("  Entry interruption 0x%llX, exit interruption 0x%llX, IDT vectoring 0x%llX\n",
                    (UINT64)ShvVmxRead(VM_ENTRY_INTR_INFO), (UINT64)ShvVmxRead(VM_EXIT_INTR_INFO),
                    (UINT64)ShvVmxRead(IDT_VECTORING_INFO));
    ShvUtilTraceDump(32);
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
ShvVmxInjectInvalidOpcode (
    _In_ PSHV_VP_STATE VpState
    )
{
    //
    // Inject #UD. RIP stays at the faulting instruction
    //
    __vmx_vmwrite(VM_ENTRY_INTR_INFO, INTR_INFO_VALID |
                                      INTR_INFO_TYPE_HARD_EXCEPTION |
                                      INTR_INFO_VECTOR_UD);
    VpState->SkipRipAdvance = TRUE;
}

VOID
ShvVmxFlushGuestMappings (
    _In_ PSHV_VP_DATA VpData
    )
{
    UINT64 capabilities;
    UINT64 descriptor[2];

    //
    // Without VPID the guest shares the OS's TLB, whose flushes cover it
    //
    if (VpData->EptControls == 0)
    {
        return;
    }

    //
    // Invalidate every mapping cached for the guest's VPID, which is 1. The
    // guest's own TLB flushes do not run when SimpleVisor initiates the
    // flush, so flush them all
    //
    capabilities = VpData->MsrData[MSR_IA32_VMX_EPT_VPID_CAP - MSR_IA32_VMX_BASIC].QuadPart;
    descriptor[0] = ShvVmxRead(VIRTUAL_PROCESSOR_ID);
    descriptor[1] = 0;
    if ((capabilities & (VMX_VPID_INVVPID_BIT | VMX_VPID_EXTENT_ALL_CONTEXT_BIT)) ==
        (VMX_VPID_INVVPID_BIT | VMX_VPID_EXTENT_ALL_CONTEXT_BIT))
    {
        ShvVmxInvvpid(VMX_INVVPID_ALL_CONTEXT, descriptor);
    }
    else if ((capabilities & (VMX_VPID_INVVPID_BIT | VMX_VPID_EXTENT_SINGLE_CONTEXT_BIT)) ==
             (VMX_VPID_INVVPID_BIT | VMX_VPID_EXTENT_SINGLE_CONTEXT_BIT))
    {
        ShvVmxInvvpid(VMX_INVVPID_SINGLE_CONTEXT, descriptor);
    }
}

VOID
ShvVmxInjectNmi (
    _In_ PSHV_VP_STATE VpState
    )
{
    //
    // Inject an NMI into the guest on the next VM entry, on the explicit
    // command of eulercheck. The guest's NMI handler takes it. An NMI
    // cannot be injected into an NMI handler, so drop the injection then
    //
    if ((ShvVmxRead(GUEST_INTERRUPTIBILITY_INFO) &
         GUEST_INTR_STATE_NMI_BLOCKING) != 0)
    {
        return;
    }
    __vmx_vmwrite(VM_ENTRY_INTR_INFO, INTR_INFO_VALID |
                                      INTR_INFO_TYPE_NMI |
                                      INTR_INFO_VECTOR_NMI);
    VpState->VpData->IpiCounts[ShvIpiNmisInjected]++;
}

VOID
ShvVmxHandleNmi (
    _In_ PSHV_VP_STATE VpState
    )
{
    //
    // NMIs exit, and the exception bitmap is empty, so this is an NMI. It is
    // SimpleVisor's flush IPI while one is on its way: flush and resume the
    // guest, which never sees it. The exit is not tied to an instruction, so
    // RIP stays where it is. NMIs stay blocked until the VM entry
    //
    // No other NMI should reach a running guest, as the only NMI the guest
    // sees is the one eulercheck asks for, which is injected rather than
    // delivered. Halt for the debugger on any other instead of dropping it,
    // as a dropped NMI would hide whatever raised it
    //
    if (((ShvVmxRead(VM_EXIT_INTR_INFO) & INTR_INFO_TYPE_MASK) == INTR_INFO_TYPE_NMI) &&
        (ShvVpTakeFlushIpi(VpState->VpData) != FALSE))
    {
        VpState->SkipRipAdvance = TRUE;
        return;
    }
    ShvVmxHandleFatalExit(VpState);
}

VOID
ShvVmxTakeHeldNmi (
    _In_ PSHV_VP_DATA VpData
    )
{
    //
    // VT-x delivers NMIs in root mode right away, through the idle IDT that
    // root mode runs with, so none is ever held
    //
    UNREFERENCED_PARAMETER(VpData);
}

VOID
ShvVmxHandleCpuid (
    _In_ PSHV_VP_STATE VpState
    )
{
    INT32 cpu_info[4];
    UINT32 reason;
    UINT64 count;

    //
    // Check for the magic CPUID sequence, and check that it is coming from
    // Ring 0. Technically we could also check the RIP and see if this falls
    // in the expected function, but we may want to allow a separate "unload"
    // driver or code at some point.
    //
    if ((VpState->VpRegs->Rax == SHV_CPUID_LEAF) &&
        (VpState->VpRegs->Rcx == SHV_CPUID_UNLOAD) &&
        ((ShvVmxRead(GUEST_CS_SELECTOR) & RPL_MASK) == DPL_SYSTEM))
    {
        VpState->ExitVm = TRUE;
        return;
    }

    //
    // Magic CPUID command to inject an NMI into this processor, which
    // eulercheck uses to exercise the NMI path. The NMI delivers on the
    // next VM entry, after the CPUID completes
    //
    if (((UINT32)VpState->VpRegs->Rax == SHV_CPUID_LEAF) &&
        ((UINT32)VpState->VpRegs->Rcx == SHV_CPUID_NMI))
    {
        ShvVmxInjectNmi(VpState);
        return;
    }

    //
    // Magic CPUID sequence to detect SimpleVisor, as vmlab\guest\eulercheck.c
    // does. A leaf of its own keeps the interface leaf of an underlying
    // hypervisor intact, such as Hv#1. EBX bit 0 reports whether this
    // processor's local APIC is in x2APIC mode, which the flush IPIs need
    //
    if (((UINT32)VpState->VpRegs->Rax == SHV_CPUID_LEAF) &&
        ((UINT32)VpState->VpRegs->Rcx == SHV_CPUID_DETECT))
    {
        VpState->VpRegs->Rax = SHV_CPUID_SIGNATURE;
        VpState->VpRegs->Rbx = (VpState->VpData->X2ApicEnabled != FALSE);
        VpState->VpRegs->Rcx = 0;
        VpState->VpRegs->Rdx = 0;
        return;
    }

    //
    // Magic CPUID sequence to read this processor's VM-Exit counters, as
    // vmlab\guest\eulercheck.c does, one exit reason (EXIT_REASON_* in vmx.h) per
    // call, given in the low 16 bits of ECX. Returns the count in EBX:EAX, and
    // the number of counters in ECX. This exit is already counted.
    //
    if (((UINT32)VpState->VpRegs->Rax == SHV_CPUID_LEAF) &&
        (((UINT32)VpState->VpRegs->Rcx & SHV_CPUID_COUNT_SELECTOR_MASK) ==
         SHV_CPUID_EXIT_COUNTS))
    {
        reason = (UINT32)VpState->VpRegs->Rcx & SHV_CPUID_COUNT_MASK;
        count = (reason < SHV_EXIT_REASON_COUNT) ?
                VpState->VpData->ExitCounts[reason] : 0;

        VpState->VpRegs->Rax = count & 0xFFFFFFFF;
        VpState->VpRegs->Rbx = count >> 32;
        VpState->VpRegs->Rcx = SHV_EXIT_REASON_COUNT;
        VpState->VpRegs->Rdx = 0;
        return;
    }

    //
    // Likewise for the TLB flush hypercalls forwarded to Hyper-V, one
    // ShvTlbHypercall per call, given in the low 16 bits of ECX
    //
    if (((UINT32)VpState->VpRegs->Rax == SHV_CPUID_LEAF) &&
        (((UINT32)VpState->VpRegs->Rcx & SHV_CPUID_COUNT_SELECTOR_MASK) ==
         SHV_CPUID_TLB_COUNTS))
    {
        reason = (UINT32)VpState->VpRegs->Rcx & SHV_CPUID_COUNT_MASK;
        count = (reason < ShvTlbHypercallCount) ?
                VpState->VpData->TlbHypercallCounts[reason] : 0;

        VpState->VpRegs->Rax = count & 0xFFFFFFFF;
        VpState->VpRegs->Rbx = count >> 32;
        VpState->VpRegs->Rcx = ShvTlbHypercallCount;
        VpState->VpRegs->Rdx = 0;
        return;
    }

    //
    // Likewise for the IPIs, one ShvIpiCounter per call, given in the low 16
    // bits of ECX
    //
    if (((UINT32)VpState->VpRegs->Rax == SHV_CPUID_LEAF) &&
        (((UINT32)VpState->VpRegs->Rcx & SHV_CPUID_COUNT_SELECTOR_MASK) ==
         SHV_CPUID_IPI_COUNTS))
    {
        reason = (UINT32)VpState->VpRegs->Rcx & SHV_CPUID_COUNT_MASK;
        count = (reason < ShvIpiCount) ?
                VpState->VpData->IpiCounts[reason] : 0;

        VpState->VpRegs->Rax = count & 0xFFFFFFFF;
        VpState->VpRegs->Rbx = count >> 32;
        VpState->VpRegs->Rcx = ShvIpiCount;
        VpState->VpRegs->Rdx = 0;
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
    if ((UINT32)VpState->VpRegs->Rax == 1)
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
    else if (((UINT32)VpState->VpRegs->Rax == HYPERV_CPUID_ENLIGHTMENT_INFO) &&
             (VpState->VpData->UnderlyingHypervisorIsHyperV != FALSE))
    {
        //
        // The hypercalls would switch and flush the address spaces of
        // SimpleVisor, rather than those of the guest on VPID 1. The
        // VM-Exit handlers make up for them, but a guest that boots after
        // SimpleVisor is better off with MOV CR3, INVLPG and IPIs instead.
        // Windows reads this at boot, so only such a guest follows it
        //
        cpu_info[0] &= ~(HYPERV_ENLIGHTMENT_ADDRESS_SPACE_SWITCH |
                         HYPERV_ENLIGHTMENT_LOCAL_TLB_FLUSH |
                         HYPERV_ENLIGHTMENT_REMOTE_TLB_FLUSH);
    }
    else if (((UINT32)VpState->VpRegs->Rax == HYPERV_CPUID_VENDOR_AND_MAX_FUNCTIONS) &&
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
    else if (((UINT32)VpState->VpRegs->Rax == HYPERV_CPUID_INTERFACE) &&
             (VpState->VpData->UnderlyingHypervisor == FALSE))
    {
        //
        // Return our interface identifier. When nested pass the underlying
        // hypervisor's through
        //
        cpu_info[0] = SHV_CPUID_SIGNATURE;
    }

    //
    // Copy the values from the logical processor registers into the VP GPRs.
    // CPUID zero-extends them into the 64-bit registers
    //
    VpState->VpRegs->Rax = (UINT32)cpu_info[0];
    VpState->VpRegs->Rbx = (UINT32)cpu_info[1];
    VpState->VpRegs->Rcx = (UINT32)cpu_info[2];
    VpState->VpRegs->Rdx = (UINT32)cpu_info[3];
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
    // The MSR bitmap traps the ICR and self IPI writes, and the MSRs outside
    // its ranges, such as the synthetic MSRs of Hyper-V, always exit. Access
    // them from root mode, which exits to the hypervisor we are nested under,
    // if any, and give the guest the #GP that the processor or that
    // hypervisor raises
    //
    // Count the IPIs the guest sends through the ICR and the self IPI
    // register, and pass the writes on. The ICR does not say what the IPI is
    // for, as the OS carries that in its own per-processor IPI mailbox, so
    // SimpleVisor cannot tell the TLB flushes from the other IPIs
    //
    msr = (UINT32)VpState->VpRegs->Rcx;
    if (VpState->ExitReason == EXIT_REASON_MSR_WRITE)
    {
        if (msr == APIC_ICR)
        {
            VpState->VpData->IpiCounts[ShvIpiIcrWrites]++;
        }
        else if (msr == APIC_SELF_IPI)
        {
            VpState->VpData->IpiCounts[ShvIpiSelfIpiWrites]++;
        }
    }

    if (VpState->ExitReason == EXIT_REASON_MSR_READ)
    {
        //
        // RDMSR returns the value in EDX:EAX, clearing the upper halves
        //
        if (ShvUtilAccessMsr(VpState->VpData, msr, FALSE, &value) == FALSE)
        {
            ShvVmxInjectGeneralProtection(VpState);
            return;
        }
        VpState->VpRegs->Rax = (UINT32)value;
        VpState->VpRegs->Rdx = value >> 32;
    }
    else
    {
        //
        // WRMSR takes the value from EDX:EAX, ignoring the upper halves
        //
        value = VpState->VpRegs->Rdx << 32 | (UINT32)VpState->VpRegs->Rax;
        if (ShvUtilAccessMsr(VpState->VpData, msr, TRUE, &value) == FALSE)
        {
            ShvVmxInjectGeneralProtection(VpState);
        }
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
ShvVmxHandleSwitchAddressSpace (
    _In_ PSHV_VP_STATE VpState
    )
{
    UINT64 addressSpace;

    //
    // HvCallSwitchVirtualAddressSpace switches the address space of the
    // calling processor to the CR3 in its input, keeping the TLB entries of
    // the old one, which SimpleVisor flushes anyway with the guest's
    // mappings. It is emulated rather than forwarded, as a forwarded
    // hypercall would switch SimpleVisor's own address space, not the
    // guest's. Without the input, fail the call, as the address space cannot
    // be guessed
    //
    if (ShvUtilGetSwitchAddressSpace(VpState, &addressSpace) == FALSE)
    {
        VpState->VpRegs->Rax = HYPERV_STATUS_INVALID_PARAMETER;
        return;
    }
    __vmx_vmwrite(GUEST_CR3, addressSpace);
    ShvVmxFlushGuestMappings(VpState->VpData);
    VpState->VpRegs->Rax = HYPERV_STATUS_SUCCESS;
}

VOID
ShvVmxHandleVmcall (
    _In_ PSHV_VP_STATE VpState
    )
{
    UINT8 flush;

    //
    // Under Hyper-V forward hypercalls from the guest to Hyper-V. The guest
    // enabled the partition-wide hypercall page before we loaded, and the
    // identity map keeps the parameter addresses valid. Root mode runs at
    // CPL 0, so raise #UD for user mode here as Hyper-V would. Otherwise
    // VMCALL fails like any other VMX instruction
    //
    if (VpState->VpData->UnderlyingHypervisorIsHyperV == FALSE)
    {
        ShvVmxHandleVmx(VpState);
        return;
    }

    if ((ShvVmxRead(GUEST_CS_SELECTOR) & RPL_MASK) != DPL_SYSTEM)
    {
        ShvVmxInjectInvalidOpcode(VpState);
        return;
    }

    //
    // The switch hypercall changes the guest's address space, which a
    // forwarded hypercall does not, so handle it separately
    //
    if ((VpState->VpRegs->Rcx & HYPERV_HYPERCALL_CODE_MASK) ==
        HvCallSwitchVirtualAddressSpace)
    {
        ShvVmxHandleSwitchAddressSpace(VpState);
        return;
    }

    flush = ShvUtilCountHypercall(VpState->VpData, VpState->VpRegs->Rcx);
    ShvVmxHypercall(VpState->VpRegs);
    if (flush != FALSE)
    {
        ShvVpFlushGuestMappings(VpState->VpData, ShvVmxOwnHypercall);
    }
}

VOID
ShvVmxIdle (
    _In_ PSHV_VP_STATE VpState
    )
{
    UINT32 vector;

    //
    // Complete the guest's HLT by halting in root mode until an interrupt
    // comes in, rather than having the guest spin through HLT exits. An HLT
    // with interrupts disabled only waits for an NMI, so just resume then.
    //
    // Inject the interrupt into the guest on VM entry. The guest's HLT is
    // complete and the STI and MOV SS blocking end with it, so the guest can
    // take the interrupt right away. Without one the guest just resumes
    //
    if ((VpState->GuestEFlags & EFLAGS_INTERRUPT_FLAG) == 0)
    {
        return;
    }

    vector = ShvUtilIdle(VpState->VpData);
    if (vector != 0)
    {
        __vmx_vmwrite(VM_ENTRY_INTR_INFO, INTR_INFO_VALID |
                                          INTR_INFO_TYPE_EXT_INTR |
                                          vector);
    }
}

VOID
ShvVmxHandleHlt (
    _In_ PSHV_VP_STATE VpState
    )
{
    //
    // Hyper-V makes HLT exiting mandatory. Resume past the HLT in the HLT
    // activity state, so the guest waits for an interrupt as before. Clear
    // the STI and MOV SS blocking of sti; hlt, which ends with the HLT and
    // which VM entry rejects in that state.
    //
    // Without the HLT activity state, or once VM entry rejected it, idle in
    // root mode instead
    //
    if (((VpState->VpData->MsrData[MSR_IA32_VMX_MISC - MSR_IA32_VMX_BASIC].LowPart &
          VMX_MISC_HLT_ACTIVITY_STATE) != 0) &&
        (VpState->VpData->HltActivityStateRejected == FALSE))
    {
        __vmx_vmwrite(GUEST_INTERRUPTIBILITY_INFO,
                      ShvVmxRead(GUEST_INTERRUPTIBILITY_INFO) &
                      ~(GUEST_INTR_STATE_STI | GUEST_INTR_STATE_MOV_SS));
        __vmx_vmwrite(GUEST_ACTIVITY_STATE, GUEST_ACTIVITY_HLT);
    }
    else
    {
        ShvVmxIdle(VpState);
    }
}

VOID
ShvVmxHandleInvalidGuestState (
    _In_ PSHV_VP_STATE VpState
    )
{
    //
    // A failed VM entry loads the host state like a VM-Exit and leaves the
    // guest state intact, so the guest can be resumed once it is fixed.
    // Nested Hyper-V rejects the HLT activity state although it reports
    // support for it. Resume the guest active instead, after idling in root
    // mode, and stop using that state on this processor. Anything else is
    // fatal
    //
    if ((VpState->VpData->HltActivityStateRejected == FALSE) &&
        (ShvVmxRead(GUEST_ACTIVITY_STATE) == GUEST_ACTIVITY_HLT))
    {
        VpState->VpData->HltActivityStateRejected = TRUE;
        __vmx_vmwrite(GUEST_ACTIVITY_STATE, GUEST_ACTIVITY_ACTIVE);
        ShvVmxIdle(VpState);

        //
        // No instruction caused this exit, and RIP is already past the HLT
        //
        VpState->SkipRipAdvance = TRUE;
        return;
    }
    ShvVmxHandleFatalExit(VpState);
}

VOID
ShvVmxHandleExit (
    _In_ PSHV_VP_STATE VpState
    )
{
    uintptr_t interruptibility;

    //
    // This is the generic VM-Exit handler. Decode the reason for the exit and
    // call the appropriate handler. As per Intel specifications, given that we
    // have requested no optional exits whatsoever, we should only see CPUID,
    // INVD, XSETBV and other VMX instructions. GETSEC cannot happen as we do
    // not run in SMX context.
    //
    // VMCALL goes to Hyper-V as a hypercall when nested under it, and
    // otherwise fails like the other VMX instructions
    //
    // RDMSR and WRMSR exit for the MSRs the bitmap traps, which root mode
    // accesses in the guest's place
    //
    // NMIs exit with no exception bitmap set for anything else. SimpleVisor's
    // flush IPIs flush the guest's mappings and are dropped, and any other
    // NMI halts for the debugger, as none should reach the guest. External
    // interrupts do not exit, and reach the guest directly
    //
    // HLT, MONITOR and MWAIT exit when an underlying hypervisor makes them
    // mandatory, as Hyper-V does. MONITOR is a hint and MWAIT may return at
    // any time, so both just complete
    //
    // A VM entry that fails on the guest state is retried without the HLT
    // activity state
    //
    // EPT violations and misconfigurations and any other exit are fatal
    //
    switch (VpState->ExitReason)
    {
    case EXIT_REASON_EXCEPTION_NMI:
        ShvVmxHandleNmi(VpState);
        break;
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
    case EXIT_REASON_HLT:
        ShvVmxHandleHlt(VpState);
        break;
    case EXIT_REASON_MONITOR_INSTRUCTION:
    case EXIT_REASON_MWAIT_INSTRUCTION:
        break;
    case EXIT_REASON_INVALID_GUEST_STATE:
        ShvVmxHandleInvalidGuestState(VpState);
        break;
    case EXIT_REASON_VMCALL:
        ShvVmxHandleVmcall(VpState);
        break;
    case EXIT_REASON_VMCLEAR:
    case EXIT_REASON_VMLAUNCH:
    case EXIT_REASON_VMPTRLD:
    case EXIT_REASON_VMPTRST:
    case EXIT_REASON_VMREAD:
    case EXIT_REASON_VMRESUME:
    case EXIT_REASON_VMWRITE:
    case EXIT_REASON_VMXOFF:
    case EXIT_REASON_VMXON:
    case EXIT_REASON_INVEPT:
    case EXIT_REASON_INVVPID:
        ShvVmxHandleVmx(VpState);
        break;
    case EXIT_REASON_EPT_VIOLATION:
    case EXIT_REASON_EPT_MISCONFIG:
    default:
        ShvVmxHandleFatalExit(VpState);
    }

    //
    // Move the instruction pointer past the exiting instruction, unless the
    // handler injected a fault. Like any instruction, it ends the STI and
    // MOV SS blocking, and single-steps with RFLAGS.TF, unless IA32_DEBUGCTL.BTF
    // limits that to branches. The pending debug exception delivers the #DB
    //
    if (VpState->SkipRipAdvance == FALSE)
    {
        VpState->GuestRip += ShvVmxRead(VM_EXIT_INSTRUCTION_LEN);
        __vmx_vmwrite(GUEST_RIP, VpState->GuestRip);

        interruptibility = ShvVmxRead(GUEST_INTERRUPTIBILITY_INFO);
        if ((interruptibility & (GUEST_INTR_STATE_STI | GUEST_INTR_STATE_MOV_SS)) != 0)
        {
            __vmx_vmwrite(GUEST_INTERRUPTIBILITY_INFO,
                          interruptibility &
                          ~(GUEST_INTR_STATE_STI | GUEST_INTR_STATE_MOV_SS));
        }

        if (((VpState->GuestEFlags & EFLAGS_TRAP_FLAG) != 0) &&
            ((ShvVmxRead(GUEST_IA32_DEBUGCTL) & DEBUG_CTL_BTF) == 0))
        {
            __vmx_vmwrite(GUEST_PENDING_DBG_EXCEPTIONS,
                          ShvVmxRead(GUEST_PENDING_DBG_EXCEPTIONS) |
                          GUEST_PENDING_DBG_BS);
        }
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
    // Count the exit, and record it in the exit trace. Higher exit reasons
    // share the last counter
    //
    if (guestContext.ExitReason < SHV_EXIT_REASON_COUNT)
    {
        vpData->ExitCounts[guestContext.ExitReason]++;
    }
    else
    {
        vpData->ExitCounts[SHV_EXIT_REASON_COUNT - 1]++;
    }
    if (guestContext.ExitReason == EXIT_REASON_MSR_READ)
    {
        ShvUtilTraceExit(vpData->ProcessorIndex, guestContext.ExitReason,
                         (UINT32)Context->Rcx, guestContext.GuestRip, 0);
    }
    else if (guestContext.ExitReason == EXIT_REASON_MSR_WRITE)
    {
        ShvUtilTraceExit(vpData->ProcessorIndex, guestContext.ExitReason,
                         (UINT32)Context->Rcx, guestContext.GuestRip,
                         (Context->Rdx << 32) | (UINT32)Context->Rax);
    }
    else if (guestContext.ExitReason == EXIT_REASON_VMCALL)
    {
        ShvUtilTraceExit(vpData->ProcessorIndex, guestContext.ExitReason,
                         (UINT32)Context->Rcx, guestContext.GuestRip, Context->Rdx);
    }
    else
    {
        ShvUtilTraceExit(vpData->ProcessorIndex, guestContext.ExitReason,
                         (UINT32)ShvVmxRead(VM_EXIT_INTR_INFO), guestContext.GuestRip,
                         ShvVmxRead(EXIT_QUALIFICATION));
    }

    //
    // Call the exit dispatcher of the backend
    //
    ShvArch->HandleExit(&guestContext);

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
        Context->Rcx = SHV_CPUID_UNLOAD_RESULT;

        //
        // This processor is leaving the guest, so stop it being a flush IPI
        // target, and take the flush IPIs still on their way, before it runs
        // the OS's code, whose NMI handler would take them for a hardware
        // failure
        //
        ShvVpSetSecondLevelRoot(vpData->ProcessorIndex, 0);
        ShvVpWithdrawFlushTarget(vpData);

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
        // The VM-Exit set DR7 to 400h and cleared IA32_DEBUGCTL, so restore
        // the guest ones, keeping its hardware breakpoints
        //
        __writedr(7, ShvVmxRead(GUEST_DR7));
        __writemsr(MSR_DEBUG_CTL, ShvVmxRead(GUEST_IA32_DEBUGCTL));

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
        ShvVmxOff(vpData);

        //
        // The OS runs on VPID 0 again, whose mappings the guest never
        // invalidated
        //
        ShvUtilFlushTlb();
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

