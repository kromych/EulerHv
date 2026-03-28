/*++

Copyright (c) Roman Kisel 2026.  All rights reserved.

Module Name:

    shvsvm.c

Abstract:

    This module implements AMD SVM (AMD-V)-specific routines.

Author:

    kromych (vdso@mailbox.org) 13-Jun-2026 - Initial version

Environment:

    Kernel mode only, IRQL DISPATCH_LEVEL.

--*/

#include "shv.h"

VOID
ShvSvmHypercall (
    _Inout_ PCONTEXT Context
    );

UINT8
ShvSvmIsAmd (
    VOID
    )
{
    INT32 cpuInfo[4];

    //
    // Check for the "AuthenticAMD" and "HygonGenuine" vendor signatures. Hygon
    // processors implement SVM as well
    //
    __cpuid(cpuInfo, 0);
    return ((cpuInfo[1] == CPUID_VENDOR_AMD_EBX) &&
            (cpuInfo[3] == CPUID_VENDOR_AMD_EDX) &&
            (cpuInfo[2] == CPUID_VENDOR_AMD_ECX)) ||
           ((cpuInfo[1] == CPUID_VENDOR_HYGON_EBX) &&
            (cpuInfo[3] == CPUID_VENDOR_HYGON_EDX) &&
            (cpuInfo[2] == CPUID_VENDOR_HYGON_ECX));
}

UINT8
ShvSvmProbe (
    _In_ PSHV_VP_DATA VpData
    )
{
    INT32 cpuInfo[4];

    //
    // Check the SVM feature bit, and that the SVM features leaf exists
    //
    __cpuid(cpuInfo, 0x80000000);
    if ((UINT32)cpuInfo[0] < CPUID_SVM_FEATURES)
    {
        return FALSE;
    }
    __cpuid(cpuInfo, CPUID_EXTENDED_FEATURES);
    if ((cpuInfo[2] & CPUID_EXTENDED_FEATURES_ECX_SVM) == 0)
    {
        return FALSE;
    }

    //
    // Check if the firmware disabled SVM. With VM_CR.LOCK clear we could clear
    // VM_CR.SVMDIS ourselves, but as with IA32_FEATURE_CONTROL on Intel we'd
    // rather not mess with it
    //
    if ((__readmsr(MSR_VM_CR) & VM_CR_SVMDIS) != 0)
    {
        return FALSE;
    }

    //
    // Capture the SVM features and the number of ASIDs, which includes ASID 0
    // of the host
    //
    __cpuid(cpuInfo, CPUID_SVM_FEATURES);
    VpData->SvmAsidCount = (UINT32)cpuInfo[1];
    VpData->SvmFeatures = (UINT32)cpuInfo[3];

    //
    // The guest needs an ASID of its own
    //
    if (VpData->SvmAsidCount < 2)
    {
        return FALSE;
    }

    //
    // Without NRIP save the VMCB does not hold the RIP past the exiting
    // instruction, and SimpleVisor does not decode instructions
    //
    if ((VpData->SvmFeatures & CPUID_SVM_FEATURES_EDX_NRIPS) == 0)
    {
        return FALSE;
    }
    return TRUE;
}

VOID
ShvSvmBuildPaging (
    _In_ PSHV_VP_DATA VpData
    )
{
    UINT32 i, j;
    SVM_NPT_PDPTE tempNpdpte;
    SVM_NPT_LARGE_PDE tempNpde;
    SVM_NPT_HUGE_PDPTE tempHugeNpdpte;
    INT32 cpuInfo[4];
    UINT32 physicalAddressBits;
    UINT32 pml4Count;

    //
    // Without nested paging the guest uses the physical addresses directly
    //
    VpData->NptPml4PhysicalAddress = 0;
    if ((VpData->SvmFeatures & CPUID_SVM_FEATURES_EDX_NP) == 0)
    {
        return;
    }

    //
    // Unlike EPT, the entries have no memory type. Leaving PWT, PCD and PAT
    // clear selects the host PAT entry 0, which is WB. WB in the nested
    // tables leaves the guest PAT type in effect, and the processor applies
    // the host MTRRs on top, so no MTRR scan is needed (APM vol. 2 15.25.8)
    //
    // Fill out the NPML4E which covers the first 512GB of RAM
    //
    VpData->Npml4[0].Present = 1;
    VpData->Npml4[0].Write = 1;
    VpData->Npml4[0].User = 1;
    VpData->Npml4[0].PageFrameNumber = ShvOsGetPhysicalAddress(&VpData->Npdpt) / PAGE_SIZE;

    //
    // Fill out a RWX PDPTE. Nested walks count as user accesses
    //
    tempNpdpte.AsUlonglong = 0;
    tempNpdpte.Present = tempNpdpte.Write = tempNpdpte.User = 1;

    //
    // Construct the nested identity map for every 1GB of RAM
    //
    __stosq((UINT64*)VpData->Npdpt, tempNpdpte.AsUlonglong, PDPTE_ENTRY_COUNT);
    for (i = 0; i < PDPTE_ENTRY_COUNT; i++)
    {
        //
        // Set the page frame number of the PDE table
        //
        VpData->Npdpt[i].PageFrameNumber = ShvOsGetPhysicalAddress(&VpData->Npde[i][0]) / PAGE_SIZE;
    }

    //
    // Fill out a RWX Large PDE
    //
    tempNpde.AsUlonglong = 0;
    tempNpde.Present = tempNpde.Write = tempNpde.User = 1;
    tempNpde.Large = 1;

    //
    // Loop every 1GB of RAM (described by the PDPTE), and identity map every
    // 2MB of it
    //
    __stosq((UINT64*)VpData->Npde, tempNpde.AsUlonglong, PDPTE_ENTRY_COUNT * PDE_ENTRY_COUNT);
    for (i = 0; i < PDPTE_ENTRY_COUNT; i++)
    {
        for (j = 0; j < PDE_ENTRY_COUNT; j++)
        {
            VpData->Npde[i][j].PageFrameNumber = (i * PDE_ENTRY_COUNT) + j;
        }
    }

    //
    // Map RAM or MMIO above 512GB with 1GB pages, which nested paging supports
    // when the processor does. As with EPT, leave the range unmapped under
    // VMWare. Whether its nested NPT handles 1GB pages is untested
    //
    __cpuid(cpuInfo, CPUID_EXTENDED_FEATURES);
    if (((cpuInfo[3] & CPUID_EXTENDED_FEATURES_EDX_PAGE_1GB) == 0) ||
        (VpData->UnderlyingHypervisorIsVmware != FALSE))
    {
        goto Done;
    }

    //
    // Each PML4 entry covers 512GB (39 bits) of the physical address width
    //
    __cpuid(cpuInfo, 0x80000000);
    if ((UINT32)cpuInfo[0] < 0x80000008)
    {
        goto Done;
    }
    __cpuid(cpuInfo, 0x80000008);
    physicalAddressBits = cpuInfo[0] & 0xFF;
    if ((physicalAddressBits <= 39) || (physicalAddressBits > 52))
    {
        goto Done;
    }
    pml4Count = 1 << (physicalAddressBits - 39);
    if (pml4Count > SHV_EPT_PML4E_COUNT)
    {
        pml4Count = SHV_EPT_PML4E_COUNT;
    }

    //
    // Fill out a RWX Huge PDPTE
    //
    tempHugeNpdpte.AsUlonglong = 0;
    tempHugeNpdpte.Present = tempHugeNpdpte.Write = tempHugeNpdpte.User = 1;
    tempHugeNpdpte.Large = 1;

    //
    // Loop every additional 512GB, and identity map every 1GB of it
    //
    for (i = 1; i < pml4Count; i++)
    {
        VpData->Npml4[i].Present = 1;
        VpData->Npml4[i].Write = 1;
        VpData->Npml4[i].User = 1;
        VpData->Npml4[i].PageFrameNumber = ShvOsGetPhysicalAddress(&VpData->NpdptHuge[i - 1]) / PAGE_SIZE;

        for (j = 0; j < PDPTE_ENTRY_COUNT; j++)
        {
            VpData->NpdptHuge[i - 1][j].AsUlonglong = tempHugeNpdpte.AsUlonglong;
            VpData->NpdptHuge[i - 1][j].PageFrameNumber = (i * PDPTE_ENTRY_COUNT) + j;
        }
    }

Done:
    //
    // There is no EPTP. The VMCB setup enables nested paging and loads this
    // into nCR3
    //
    VpData->NptPml4PhysicalAddress = ShvOsGetPhysicalAddress(&VpData->Npml4);
}

VOID
ShvSvmLeaveRootMode (
    VOID
    )
{
    //
    // Disable the SVM instructions and forget the host save area
    //
    __writemsr(MSR_EFER, __readmsr(MSR_EFER) & ~EFER_SVME);
    __writemsr(MSR_VM_HSAVE_PA, 0);
}

UINT8
ShvSvmEnterRootModeOnVp (
    _In_ PSHV_VP_DATA VpData
    )
{
    UINT64 efer;

    //
    // Some other hypervisor already uses SVM on this processor, and owns the
    // host save area. Fail as VMXON does in VMX operation
    //
    efer = __readmsr(MSR_EFER);
    if ((efer & EFER_SVME) != 0)
    {
        return FALSE;
    }

    //
    // Unlike VT-x there is no VMXON region, revision ID or CR0 and CR4 fixed
    // bits. Enable the SVM instructions, and give VMRUN the 4KB host save area
    //
    VpData->HostSaveAreaPhysicalAddress = ShvOsGetPhysicalAddress(VpData->HostSaveArea);
    VpData->MsrBitmapPhysicalAddress = ShvOsGetPhysicalAddress(VpData->MsrBitmap);
    __writemsr(MSR_EFER, efer | EFER_SVME);
    __writemsr(MSR_VM_HSAVE_PA, VpData->HostSaveAreaPhysicalAddress);
    return TRUE;
}

static
VOID
ShvSvmSetMsrPermission (
    _Inout_ UINT8* MsrBitmap,
    _In_ UINT32 Msr
    )
{
    UINT32 offset;
    UINT32 bitPos;

    //
    // The MSR permission map has three 2KB sections for MSRs 0x0000-0x1FFF,
    // 0xC000_0000-0xC000_1FFF and 0xC001_0000-0xC001_1FFF. Each MSR occupies
    // two bits: bit 0 is the read intercept and bit 1 is the write intercept
    //
    if (Msr <= 0x1FFF)
    {
        offset = 0;
    }
    else if ((Msr >= 0xC0000000) && (Msr <= 0xC0001FFF))
    {
        offset = 0x800;
        Msr -= 0xC0000000;
    }
    else if ((Msr >= 0xC0010000) && (Msr <= 0xC0011FFF))
    {
        offset = 0x1000;
        Msr -= 0xC0010000;
    }
    else
    {
        return;
    }

    bitPos = Msr * 2;
    offset += bitPos / 8;
    bitPos %= 8;
    MsrBitmap[offset] |= (UINT8)((1 << bitPos) | (1 << (bitPos + 1)));
}

static
VOID
ShvSvmLoadSegment (
    _Out_ PSVM_VMCB_SEGMENT VmcbSeg,
    _In_ PVMX_GDTENTRY64 GdtEntry
    )
{
    //
    // Move AVL, L, D/B and G from bits 12-15 down to bits 8-11
    //
    VmcbSeg->Selector = GdtEntry->Selector;
    VmcbSeg->Attrib = (UINT16)((GdtEntry->AccessRights & 0xFF) |
                                ((GdtEntry->AccessRights >> 4) & 0xF00));
    VmcbSeg->Limit = GdtEntry->Limit;
    VmcbSeg->Base = GdtEntry->Base;
}

VOID
ShvSvmSetupVmcbForVp (
    _In_ PSHV_VP_DATA VpData
    )
{
    PSHV_SPECIAL_REGISTERS state = &VpData->SpecialRegisters;
    PCONTEXT context = &VpData->ContextFrame;
    PSVM_VMCB vmcb = &VpData->Vmcb;
    VMX_GDTENTRY64 gdtEntry;

    //
    // Intercept EFER, VM_CR and VM_HSAVE_PA, and pass the other MSRs through
    //
    ShvSvmSetMsrPermission(VpData->MsrBitmap, MSR_EFER);
    ShvSvmSetMsrPermission(VpData->MsrBitmap, MSR_VM_CR);
    ShvSvmSetMsrPermission(VpData->MsrBitmap, MSR_VM_HSAVE_PA);

    //
    // Intercept CPUID, INVD, INVLPGA, MSR accesses through the permission
    // map, XSETBV, and the SVM instructions. VMRUN must always be intercepted
    //
    vmcb->InterceptMisc1 = SVM_INTERCEPT_CPUID |
                           SVM_INTERCEPT_INVD |
                           SVM_INTERCEPT_INVLPGA |
                           SVM_INTERCEPT_MSR_PROT;
    vmcb->InterceptMisc2 = SVM_INTERCEPT_VMRUN |
                           SVM_INTERCEPT_VMMCALL |
                           SVM_INTERCEPT_VMLOAD |
                           SVM_INTERCEPT_VMSAVE |
                           SVM_INTERCEPT_STGI |
                           SVM_INTERCEPT_CLGI |
                           SVM_INTERCEPT_SKINIT |
                           SVM_INTERCEPT_XSETBV;

    //
    // Point the MSR permission map at the 8KB region
    //
    vmcb->MsrpmBasePa = VpData->MsrBitmapPhysicalAddress;

    //
    // ASID 0 belongs to the host. Flush the TLB on the first VMRUN
    //
    vmcb->GuestAsid = 1;
    vmcb->TlbControl = SVM_TLB_CONTROL_FLUSH_ALL;

    //
    // Enable nested paging and load the nested page table root, if the
    // processor supports it
    //
    if (VpData->NptPml4PhysicalAddress != 0)
    {
        vmcb->NpEnable = 1;
        vmcb->NCr3 = VpData->NptPml4PhysicalAddress;
    }

    //
    // Load the CS Segment (Ring 0 Code)
    //
    ShvUtilConvertGdtEntry(state->Gdtr.Base, context->SegCs, &gdtEntry);
    ShvSvmLoadSegment(&vmcb->Cs, &gdtEntry);

    //
    // Load the SS Segment (Ring 0 Data)
    //
    ShvUtilConvertGdtEntry(state->Gdtr.Base, context->SegSs, &gdtEntry);
    ShvSvmLoadSegment(&vmcb->Ss, &gdtEntry);

    //
    // Load the DS Segment (Ring 3 Data)
    //
    ShvUtilConvertGdtEntry(state->Gdtr.Base, context->SegDs, &gdtEntry);
    ShvSvmLoadSegment(&vmcb->Ds, &gdtEntry);

    //
    // Load the ES Segment (Ring 3 Data)
    //
    ShvUtilConvertGdtEntry(state->Gdtr.Base, context->SegEs, &gdtEntry);
    ShvSvmLoadSegment(&vmcb->Es, &gdtEntry);

    //
    // Load the FS Segment (Ring 3 Compatibility-Mode TEB)
    //
    ShvUtilConvertGdtEntry(state->Gdtr.Base, context->SegFs, &gdtEntry);
    ShvSvmLoadSegment(&vmcb->Fs, &gdtEntry);

    //
    // Load the GS Segment (MSR-based in Long Mode)
    //
    ShvUtilConvertGdtEntry(state->Gdtr.Base, context->SegGs, &gdtEntry);
    ShvSvmLoadSegment(&vmcb->Gs, &gdtEntry);
    vmcb->Gs.Base = state->MsrGsBase;

    //
    // Load the Task Register (Ring 0 TSS)
    //
    ShvUtilConvertGdtEntry(state->Gdtr.Base, state->Tr, &gdtEntry);
    ShvSvmLoadSegment(&vmcb->Tr, &gdtEntry);

    //
    // Load the Local Descriptor Table
    //
    ShvUtilConvertGdtEntry(state->Gdtr.Base, state->Ldtr, &gdtEntry);
    ShvSvmLoadSegment(&vmcb->Ldtr, &gdtEntry);

    //
    // Load the GDT and IDT. The selector and attrib fields are reserved
    //
    vmcb->Gdtr.Base = (UINT64)state->Gdtr.Base;
    vmcb->Gdtr.Limit = state->Gdtr.Limit;
    vmcb->Idtr.Base = (UINT64)state->Idtr.Base;
    vmcb->Idtr.Limit = state->Idtr.Limit;

    //
    // Load control registers and EFER, which needs SVME for VMRUN
    //
    vmcb->Cr0 = state->Cr0;
    vmcb->Cr3 = state->Cr3;
    vmcb->Cr4 = state->Cr4;
    vmcb->Efer = __readmsr(MSR_EFER) | EFER_SVME;

    //
    // Load debug registers
    //
    vmcb->Dr7 = state->KernelDr7;
    vmcb->Dr6 = __readdr(6);

    //
    // Load the guest stack, instruction pointer and rflags, matching the VT-x
    // VMCS setup. The guest resumes at ShvVpRestoreAfterLaunch with interrupts
    // off. The 8 byte bias stands in for a return address, which keeps RSP
    // ABI-aligned on entry
    //
    vmcb->Rsp = (UINT64)VpData->ShvStackLimit + KERNEL_STACK_SIZE -
                sizeof(CONTEXT) - sizeof(UINT64);
    vmcb->Rip = (UINT64)ShvVpRestoreAfterLaunch;
    vmcb->Rflags = context->EFlags & ~EFLAGS_INTERRUPT_FLAG;
    vmcb->Rax = context->Rax;

    //
    // Load every VMCB field on the first VMRUN
    //
    vmcb->VmcbCleanBits = 0;
}

INT32
ShvSvmLaunch (
    _In_ PSHV_VP_DATA VpData
    )
{
    UNREFERENCED_PARAMETER(VpData);

    //
    // There is no VMRUN loop yet. Leave root mode, as a failed VMLAUNCH does
    //
    ShvSvmLeaveRootMode();
    return SHV_STATUS_NOT_AVAILABLE;
}

UINT32
FORCEINLINE
ShvSvmExitIndex (
    _In_ UINT16 ExitCode
    )
{
    //
    // The exit codes are sparse. Count 0x72-0x8D and NPF, and let the others
    // share the last counter
    //
    if ((ExitCode >= VMEXIT_CPUID) && (ExitCode <= VMEXIT_XSETBV))
    {
        return ExitCode - VMEXIT_CPUID;
    }
    if (ExitCode == VMEXIT_NPF)
    {
        return VMEXIT_XSETBV - VMEXIT_CPUID + 1;
    }
    return SHV_EXIT_REASON_COUNT - 1;
}

DECLSPEC_NORETURN
VOID
ShvSvmHalt (
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
ShvSvmHandleFatalExit (
    _In_ PSHV_VP_STATE VpState
    )
{
    PSHV_VP_DATA vpData = VpState->VpData;
    PSVM_VMCB vmcb = &vpData->Vmcb;

    //
    // An exit SimpleVisor cannot handle, such as a nested page fault outside
    // the identity map. Save the details for the debugger and halt. For a
    // nested page fault the GPA is in ExitInfo2
    //
    vpData->FatalExitReason = vmcb->ExitCode;
    vpData->FatalExitQualification = vmcb->ExitInfo1;
    vpData->FatalGuestPhysicalAddress = vmcb->ExitInfo2;
    vpData->FatalGuestLinearAddress = 0;
    vpData->FatalGuestRip = vmcb->Rip;

    ShvOsDebugPrint("SimpleVisor: fatal #VMEXIT on CPU %d, VP data 0x%llX\n",
                    vpData->ProcessorIndex, (UINT64)vpData);
    ShvOsDebugPrint("  ExitCode 0x%llX, ExitInfo1 0x%llX, ExitInfo2 0x%llX\n",
                    vmcb->ExitCode, vmcb->ExitInfo1, vmcb->ExitInfo2);
    ShvOsDebugPrint("  ExitIntInfo 0x%llX, RIP 0x%llX, RSP 0x%llX, RFLAGS 0x%llX\n",
                    vmcb->ExitIntInfo, vmcb->Rip, vmcb->Rsp, vmcb->Rflags);
    ShvOsDebugPrint("  CPL %u, EFER 0x%llX\n",
                    (UINT32)vmcb->Cpl, vmcb->Efer);
    ShvSvmHalt();
}

VOID
ShvSvmInjectGeneralProtection (
    _In_ PSHV_VP_STATE VpState
    )
{
    //
    // Inject #GP(0). RIP stays at the faulting instruction
    //
    VpState->VpData->Vmcb.EventInj = SVM_EVENTINJ_VALID |
                                     SVM_EVENTINJ_DELIVER_ERROR_CODE |
                                     SVM_EVENTINJ_TYPE_EXCEPTION |
                                     INTR_INFO_VECTOR_GP;
    VpState->SkipRipAdvance = TRUE;
}

VOID
ShvSvmInjectInvalidOpcode (
    _In_ PSHV_VP_STATE VpState
    )
{
    //
    // Inject #UD. RIP stays at the faulting instruction
    //
    VpState->VpData->Vmcb.EventInj = SVM_EVENTINJ_VALID |
                                     SVM_EVENTINJ_TYPE_EXCEPTION |
                                     INTR_INFO_VECTOR_UD;
    VpState->SkipRipAdvance = TRUE;
}

VOID
ShvSvmHandleCpuid (
    _In_ PSHV_VP_STATE VpState
    )
{
    INT32 cpu_info[4];
    UINT32 reason;
    UINT64 count;

    //
    // Magic CPUID sequence to unload SimpleVisor, from ring 0
    //
    if ((VpState->VpRegs->Rax == 0x41414141) &&
        (VpState->VpRegs->Rcx == 0x42424242) &&
        (VpState->VpData->Vmcb.Cpl == DPL_SYSTEM))
    {
        VpState->ExitVm = TRUE;
        return;
    }

    //
    // Magic CPUID sequence to detect SimpleVisor
    //
    if (((UINT32)VpState->VpRegs->Rax == 0x41414141) &&
        ((UINT32)VpState->VpRegs->Rcx == 0x44444444))
    {
        VpState->VpRegs->Rax = ' vhS';
        VpState->VpRegs->Rbx = 0;
        VpState->VpRegs->Rcx = 0;
        VpState->VpRegs->Rdx = 0;
        return;
    }

    //
    // Magic CPUID sequence to read this processor's #VMEXIT counters, one
    // ShvSvmExitIndex per call, given in the low 16 bits of ECX
    //
    if (((UINT32)VpState->VpRegs->Rax == 0x41414141) &&
        (((UINT32)VpState->VpRegs->Rcx & 0xFFFF0000) == 0x45450000))
    {
        reason = (UINT32)VpState->VpRegs->Rcx & 0xFFFF;
        count = (reason < SHV_EXIT_REASON_COUNT) ?
                VpState->VpData->ExitCounts[reason] : 0;

        VpState->VpRegs->Rax = count & 0xFFFFFFFF;
        VpState->VpRegs->Rbx = count >> 32;
        VpState->VpRegs->Rcx = SHV_EXIT_REASON_COUNT;
        VpState->VpRegs->Rdx = 0;
        return;
    }

    //
    // Issue the CPUID to the logical processor
    //
    __cpuidex(cpu_info, (INT32)VpState->VpRegs->Rax, (INT32)VpState->VpRegs->Rcx);

    if (VpState->VpRegs->Rax == 1)
    {
        //
        // Set the Hypervisor Present-bit
        //
        cpu_info[2] |= HYPERV_HYPERVISOR_PRESENT_BIT;
    }
    else if (VpState->VpRegs->Rax == CPUID_EXTENDED_FEATURES)
    {
        //
        // Hide SVM, as the SVM instructions raise #UD in the guest
        //
        cpu_info[2] &= ~CPUID_EXTENDED_FEATURES_ECX_SVM;
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
        // Return the SimpleVisor vendor. When nested pass the underlying
        // hypervisor's through
        //
        cpu_info[0] = HYPERV_CPUID_INTERFACE;
        cpu_info[1] = 'pmiS';
        cpu_info[2] = 'iVel';
        cpu_info[3] = 'ros';
    }
    else if ((VpState->VpRegs->Rax == HYPERV_CPUID_INTERFACE) &&
             (VpState->VpData->UnderlyingHypervisor == FALSE))
    {
        //
        // Return the SimpleVisor interface identifier
        //
        cpu_info[0] = ' vhS';
    }

    VpState->VpRegs->Rax = cpu_info[0];
    VpState->VpRegs->Rbx = cpu_info[1];
    VpState->VpRegs->Rcx = cpu_info[2];
    VpState->VpRegs->Rdx = cpu_info[3];
}

VOID
ShvSvmHandleInvd (
    VOID
    )
{
    //
    // Same as the VT-x handler: WBINVD in place of INVD
    //
    __wbinvd();
}

VOID
ShvSvmHandleXsetbv (
    _In_ PSHV_VP_STATE VpState
    )
{
    INT32 cpu_info[4];
    UINT64 xcr0;
    UINT64 supported;

    //
    // Same validation as the VT-x handler: reject invalid XCR0 values
    //
    xcr0 = VpState->VpRegs->Rdx << 32 | (UINT32)VpState->VpRegs->Rax;
    __cpuidex(cpu_info, 0xD, 0);
    supported = (UINT64)(UINT32)cpu_info[3] << 32 | (UINT32)cpu_info[0];

    if (((UINT32)VpState->VpRegs->Rcx != 0) ||
        ((xcr0 & ~supported) != 0) ||
        ((xcr0 & XCR0_X87) == 0) ||
        (((xcr0 & XCR0_AVX) != 0) &&
         ((xcr0 & XCR0_SSE) == 0)) ||
        (((xcr0 & XCR0_BNDREG) != 0) !=
         ((xcr0 & XCR0_BNDCSR) != 0)) ||
        (((xcr0 & XCR0_AVX512) != 0) &&
         ((xcr0 & (XCR0_AVX512 | XCR0_AVX | XCR0_SSE)) !=
          (XCR0_AVX512 | XCR0_AVX | XCR0_SSE))) ||
        (((xcr0 & XCR0_TILECFG) != 0) !=
         ((xcr0 & XCR0_TILEDATA) != 0)))
    {
        ShvSvmInjectGeneralProtection(VpState);
        return;
    }

    _xsetbv((UINT32)VpState->VpRegs->Rcx, xcr0);
}

VOID
ShvSvmHandleMsr (
    _In_ PSHV_VP_STATE VpState
    )
{
    PSVM_VMCB vmcb;
    UINT32 msr;
    UINT64 value;

    vmcb = &VpState->VpData->Vmcb;
    msr = (UINT32)VpState->VpRegs->Rcx;

    //
    // VMRUN needs SVME in the guest EFER. Keep it set, and hide it from the
    // guest
    //
    if (msr == MSR_EFER)
    {
        if (vmcb->ExitInfo1 != 0)
        {
            value = VpState->VpRegs->Rdx << 32 | (UINT32)VpState->VpRegs->Rax;
            vmcb->Efer = value | EFER_SVME;
            vmcb->VmcbCleanBits &= ~SVM_VMCB_CLEAN_CRX;
        }
        else
        {
            value = vmcb->Efer & ~EFER_SVME;
            VpState->VpRegs->Rax = (UINT32)value;
            VpState->VpRegs->Rdx = value >> 32;
        }
        return;
    }

    //
    // VM_HSAVE_PA and VM_CR belong to the hypervisor, not the guest
    //
    if ((msr == MSR_VM_HSAVE_PA) || (msr == MSR_VM_CR))
    {
        ShvSvmInjectGeneralProtection(VpState);
        return;
    }

    //
    // Under Hyper-V pass its synthetic MSRs through as on VT-x. Others
    // outside the MSR permission map raise #GP
    //
    if ((VpState->VpData->UnderlyingHypervisorIsHyperV != FALSE) &&
        (msr >= HYPERV_SYNTHETIC_MSR_MIN) &&
        (msr <= HYPERV_SYNTHETIC_MSR_MAX))
    {
        if (vmcb->ExitInfo1 != 0)
        {
            value = VpState->VpRegs->Rdx << 32 | (UINT32)VpState->VpRegs->Rax;
            __writemsr(msr, value);
        }
        else
        {
            value = __readmsr(msr);
            VpState->VpRegs->Rax = (UINT32)value;
            VpState->VpRegs->Rdx = value >> 32;
        }
        return;
    }

    ShvSvmInjectGeneralProtection(VpState);
}

VOID
ShvSvmHandleSvmInstruction (
    _In_ PSHV_VP_STATE VpState
    )
{
    //
    // The guest cannot use SVM
    //
    ShvSvmInjectInvalidOpcode(VpState);
}

VOID
ShvSvmHandleVmmcall (
    _In_ PSHV_VP_STATE VpState
    )
{
    //
    // Under Hyper-V forward hypercalls to Hyper-V, raising #UD for user mode
    // as Hyper-V would. Otherwise VMMCALL fails like the SVM instructions
    //
    if (VpState->VpData->UnderlyingHypervisorIsHyperV == FALSE)
    {
        ShvSvmHandleSvmInstruction(VpState);
        return;
    }

    if (VpState->VpData->Vmcb.Cpl != DPL_SYSTEM)
    {
        ShvSvmInjectInvalidOpcode(VpState);
        return;
    }

    ShvSvmHypercall(VpState->VpRegs);
}

VOID
ShvSvmHandleExit (
    _In_ PSHV_VP_STATE VpState
    )
{
    PSVM_VMCB vmcb = &VpState->VpData->Vmcb;

    //
    // Unlike VT-x, nested Hyper-V does not force HLT, MONITOR and MWAIT
    // exits, and there is no activity state to fail VM entry on
    //
    switch (VpState->ExitReason)
    {
    case VMEXIT_CPUID:
        ShvSvmHandleCpuid(VpState);
        break;
    case VMEXIT_INVD:
        ShvSvmHandleInvd();
        break;
    case VMEXIT_XSETBV:
        ShvSvmHandleXsetbv(VpState);
        break;
    case VMEXIT_MSR:
        ShvSvmHandleMsr(VpState);
        break;
    case VMEXIT_VMMCALL:
        ShvSvmHandleVmmcall(VpState);
        break;
    case VMEXIT_VMRUN:
    case VMEXIT_VMLOAD:
    case VMEXIT_VMSAVE:
    case VMEXIT_STGI:
    case VMEXIT_CLGI:
    case VMEXIT_SKINIT:
    case VMEXIT_INVLPGA:
        ShvSvmHandleSvmInstruction(VpState);
        break;
    case VMEXIT_NPF:
    default:
        ShvSvmHandleFatalExit(VpState);
    }

    //
    // Move the instruction pointer past the exiting instruction, unless the
    // handler injected a fault
    //
    if (VpState->SkipRipAdvance == FALSE)
    {
        VpState->GuestRip = vmcb->NextRip;
        vmcb->Rip = VpState->GuestRip;
    }
}

//
// The SVM backend. The probe, the nested page tables, root mode, VMCB setup
// and exit handling are implemented. The VMRUN loop is next
//
const SHV_ARCH ShvSvmArch =
{
    ShvSvmProbe,
    ShvSvmBuildPaging,
    ShvSvmEnterRootModeOnVp,
    ShvSvmSetupVmcbForVp,
    ShvSvmLaunch,
    ShvSvmHandleExit,
};
