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

//
// The top of the hypervisor stack. ShvSvmVmrun runs with RSP at the CONTEXT,
// and finds the host VMCB address right after it
//
typedef struct _SHV_SVM_HOST_FRAME
{
    CONTEXT Context;
    UINT64 HostVmcbPhysicalAddress;
} SHV_SVM_HOST_FRAME, *PSHV_SVM_HOST_FRAME;

C_ASSERT(sizeof(CONTEXT) == 0x4D0);

//
// SvmNmiState: the guest takes NMIs, runs the NMI handler SimpleVisor
// injected one into, or runs that handler's intercepted IRET
//
#define SHV_SVM_NMI_UNBLOCKED       0
#define SHV_SVM_NMI_IN_HANDLER      1
#define SHV_SVM_NMI_IRET_RUNNING    2

VOID
ShvSvmVmrun (
    VOID
    );

VOID
ShvSvmHypercall (
    _Inout_ PCONTEXT Context
    );

SHV_HYPERCALL ShvSvmOwnHypercall;

DECLSPEC_NORETURN
VOID
ShvSvmHandleFatalExit (
    _In_ PSHV_VP_STATE VpState
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

static
UINT64
ShvSvmGetEferValidBits (
    VOID
    )
{
    INT32 cpuInfo[4];
    UINT32 maxLeaf;
    UINT64 validBits;

    //
    // Long mode and SYSCALL are always there. SVM looks disabled by the
    // firmware, so SVME is reserved. Bits the OS already set are valid
    // regardless
    //
    validBits = EFER_SCE | EFER_LME | EFER_LMA;
    validBits |= __readmsr(MSR_EFER) & ~EFER_SVME;

    __cpuid(cpuInfo, 0x80000000);
    maxLeaf = (UINT32)cpuInfo[0];

    __cpuid(cpuInfo, CPUID_EXTENDED_FEATURES);
    if ((cpuInfo[3] & CPUID_EXTENDED_FEATURES_EDX_NX) != 0)
    {
        validBits |= EFER_NXE;
    }
    if ((cpuInfo[3] & CPUID_EXTENDED_FEATURES_EDX_FFXSR) != 0)
    {
        validBits |= EFER_FFXSR;
    }
    if ((cpuInfo[2] & CPUID_EXTENDED_FEATURES_ECX_TCE) != 0)
    {
        validBits |= EFER_TCE;
    }

    if (maxLeaf >= CPUID_ADDRESS_SIZES)
    {
        __cpuid(cpuInfo, CPUID_ADDRESS_SIZES);
        if ((cpuInfo[1] & CPUID_ADDRESS_SIZES_EBX_NO_LMSLE) == 0)
        {
            validBits |= EFER_LMSLE;
        }
        if ((cpuInfo[1] & CPUID_ADDRESS_SIZES_EBX_MCOMMIT) != 0)
        {
            validBits |= EFER_MCOMMIT;
        }
        if ((cpuInfo[1] & CPUID_ADDRESS_SIZES_EBX_INT_WBINVD) != 0)
        {
            validBits |= EFER_INTWB;
        }
    }

    if (maxLeaf >= CPUID_EXTENDED_FEATURES_2)
    {
        __cpuid(cpuInfo, CPUID_EXTENDED_FEATURES_2);
        if ((cpuInfo[0] & CPUID_EXTENDED_FEATURES_2_EAX_UAI) != 0)
        {
            validBits |= EFER_UAIE;
        }
        if ((cpuInfo[0] & CPUID_EXTENDED_FEATURES_2_EAX_AUTO_IBRS) != 0)
        {
            validBits |= EFER_AIBRSE;
        }
    }
    return validBits;
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

    VpData->SvmEferValidBits = ShvSvmGetEferValidBits();
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
    _In_ UINT32 Msr,
    _In_ UINT8 WriteOnly
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
    MsrBitmap[offset] |= (UINT8)((WriteOnly == FALSE ? (1 << bitPos) : 0) |
                                 (1 << (bitPos + 1)));
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
    // Intercept EFER, VM_CR and VM_HSAVE_PA, and pass the other MSRs through.
    // Intercept the ICR and self IPI register writes, which send IPIs, and
    // pass their reads through
    //
    ShvSvmSetMsrPermission(VpData->MsrBitmap, MSR_EFER, FALSE);
    ShvSvmSetMsrPermission(VpData->MsrBitmap, MSR_VM_CR, FALSE);
    ShvSvmSetMsrPermission(VpData->MsrBitmap, MSR_VM_HSAVE_PA, FALSE);
    ShvSvmSetMsrPermission(VpData->MsrBitmap, APIC_ICR, TRUE);
    ShvSvmSetMsrPermission(VpData->MsrBitmap, APIC_SELF_IPI, TRUE);

    //
    // Intercept CPUID, HLT, the MSRs in the permission map and the SVM
    // instructions. VMRUN must always be intercepted. Intercept INVD as well,
    // which VT-x always exits on, as it would throw away the cached writes
    // of the host along with the guest's. NMIs are intercepted too, as
    // SimpleVisor's flush IPIs are NMIs that the guest must not take, and
    // any other NMI is injected into the guest once it can take one. IRET
    // and #DB are intercepted only to tell when that is. The guest takes its
    // interrupts directly
    //
    vmcb->InterceptMisc1 = SVM_INTERCEPT_NMI |
                           SVM_INTERCEPT_CPUID |
                           SVM_INTERCEPT_INVD |
                           SVM_INTERCEPT_HLT |
                           SVM_INTERCEPT_INVLPGA |
                           SVM_INTERCEPT_MSR_PROT;
    vmcb->InterceptMisc2 = SVM_INTERCEPT_VMRUN |
                           SVM_INTERCEPT_VMMCALL |
                           SVM_INTERCEPT_VMLOAD |
                           SVM_INTERCEPT_VMSAVE |
                           SVM_INTERCEPT_STGI |
                           SVM_INTERCEPT_CLGI |
                           SVM_INTERCEPT_SKINIT;

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
    // The guest takes its interrupts directly, with the OS's TPR, as
    // V_INTR_MASKING is clear. The processor then ignores V_TPR, but Hyper-V
    // runs a nested guest at the TPR in V_TPR, writing it back at each exit.
    // Left at 0, the guest would resume at PASSIVE_LEVEL in the DPC that
    // virtualizes this processor, and take DPC interrupts and switch threads
    // in it, on the processor's DPC stack. So start it at the OS's TPR
    //
    vmcb->VirtualInterrupt = (UINT32)__readcr8() & SVM_V_TPR_MASK;

    //
    // Enable nested paging and load the nested page table root, if the
    // processor supports it
    //
    if (VpData->NptPml4PhysicalAddress != 0)
    {
        vmcb->NpEnable = SVM_NP_ENABLE;
        vmcb->NCr3 = VpData->NptPml4PhysicalAddress;
        ShvVpSetSecondLevelRoot(VpData->ProcessorIndex, VpData->NptPml4PhysicalAddress);
    }

    //
    // Let the guest run INVLPGB and TLBSYNC, which raise #UD in it otherwise.
    // The OS found them in CPUID before SimpleVisor loaded, so hiding them
    // now would not stop it from using them. INVLPGB in the guest flushes
    // the guest's ASID whatever ASID it names, and every processor runs the
    // guest on ASID 1, so it flushes the guest's translations everywhere, as
    // it would without SimpleVisor
    //
    if ((VpData->SvmFeatures & CPUID_SVM_FEATURES_EDX_TLBI_CTL) != 0)
    {
        vmcb->NpEnable |= SVM_NP_ENABLE_INVLPGB;
    }

    //
    // Capture FS, GS, TR, LDTR and the system call MSRs, which VMLOAD loads
    // before each VMRUN
    //
    __svm_vmsave(ShvOsGetPhysicalAddress(&VpData->Vmcb));

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
    // Load the GDT and IDT
    //
    vmcb->Gdtr.Base = (UINT64)state->Gdtr.Base;
    vmcb->Gdtr.Limit = state->Gdtr.Limit;
    vmcb->Idtr.Base = (UINT64)state->Idtr.Base;
    vmcb->Idtr.Limit = state->Idtr.Limit;
    ShvUtilBuildIdleIdt(VpData);

    //
    // Load control registers, CR2, PAT and the CPL, and EFER, which needs
    // SVME for VMRUN
    //
    vmcb->Cr0 = state->Cr0;
    vmcb->Cr3 = state->Cr3;
    vmcb->Cr4 = state->Cr4;
    vmcb->Cr2 = __readcr2();
    vmcb->Efer = __readmsr(MSR_EFER) | EFER_SVME;
    vmcb->Pat = __readmsr(MSR_PAT);
    vmcb->Cpl = 0;

    //
    // With V_SPEC_CTRL, VMRUN loads SPEC_CTRL from the VMCB, so give the guest
    // the speculation controls the OS set rather than none
    //
    if ((VpData->SvmFeatures & CPUID_SVM_FEATURES_EDX_V_SPEC_CTRL) != 0)
    {
        vmcb->SpecCtrl = __readmsr(MSR_SPEC_CTRL);
    }

    //
    // Load debug registers
    //
    vmcb->Dr7 = state->KernelDr7;
    vmcb->Dr6 = __readdr(6);

    //
    // Load the guest stack, instruction pointer and rflags. The guest resumes
    // where the context was captured
    //
    vmcb->Rsp = context->Rsp;
    vmcb->Rip = context->Rip;
    vmcb->Rflags = context->EFlags;
    vmcb->Rax = context->Rax;

    //
    // SVM does not intercept UMONITOR, UMWAIT and TPAUSE, so keep WAITPKG
    //
    VpData->UserWaitPauseEnabled = TRUE;

    //
    // Load every VMCB field on the first VMRUN
    //
    vmcb->VmcbCleanBits = 0;
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
ShvSvmEntryHandler (
    _In_ PCONTEXT Context
    )
{
    SHV_VP_STATE guestContext;
    PSHV_VP_DATA vpData;
    PSVM_VMCB vmcb;
    UINT32 index;

    //
    // The host frame sits at the top of the hypervisor stack, and the VP data
    // at its bottom. A clobbered canary means the stack overflowed into it
    //
    vpData = (PSHV_VP_DATA)((uintptr_t)((PSHV_SVM_HOST_FRAME)Context + 1) -
                            KERNEL_STACK_SIZE);
    if (vpData->StackCanary != SHV_STACK_CANARY)
    {
        ShvSvmHalt();
    }
    vmcb = &vpData->Vmcb;

    //
    // Build the VP state, and record the exit in the exit trace. The guest
    // RAX is in the VMCB
    //
    Context->Rax = vmcb->Rax;
    guestContext.GuestEFlags = vmcb->Rflags;
    guestContext.GuestRip = vmcb->Rip;
    guestContext.GuestRsp = vmcb->Rsp;
    guestContext.ExitReason = (UINT16)vmcb->ExitCode;
    guestContext.VpRegs = Context;
    guestContext.VpData = vpData;
    guestContext.ExitVm = FALSE;
    guestContext.SkipRipAdvance = FALSE;
    if (guestContext.ExitReason == VMEXIT_MSR)
    {
        ShvUtilTraceExit(vpData->ProcessorIndex, guestContext.ExitReason,
                         (UINT32)Context->Rcx, guestContext.GuestRip,
                         (vmcb->ExitInfo1 != 0) ? ((Context->Rdx << 32) | (UINT32)Context->Rax) : 0);
    }
    else if (guestContext.ExitReason == VMEXIT_VMMCALL)
    {
        ShvUtilTraceExit(vpData->ProcessorIndex, guestContext.ExitReason,
                         (UINT32)Context->Rcx, guestContext.GuestRip, Context->Rdx);
    }
    else
    {
        ShvUtilTraceExit(vpData->ProcessorIndex, guestContext.ExitReason,
                         (UINT32)vmcb->ExitIntInfo, guestContext.GuestRip, vmcb->ExitInfo1);
    }

    //
    // VMRUN rejected the VMCB. If the guest never ran, return to ShvSvmLaunch,
    // whose stack is still intact. Otherwise the guest cannot be resumed
    //
    if (vmcb->ExitCode == VMEXIT_INVALID)
    {
        if (vpData->SvmLaunchContext != NULL)
        {
            vpData->SvmLaunchFailed = TRUE;
            __svm_stgi();
            ShvOsRestoreContext(vpData->SvmLaunchContext);
        }
        ShvSvmHandleFatalExit(&guestContext);
    }

    //
    // The guest ran, so there is nothing to return to, and its TLB entries
    // for the ASID are current
    //
    vpData->SvmLaunchContext = NULL;
    vmcb->TlbControl = SVM_TLB_CONTROL_DO_NOTHING;

    //
    // Nothing changed the VMCB since VMRUN loaded it, so let the next VMRUN
    // skip reloading the cached groups. The handlers clear the bits of the
    // groups they change
    //
    if ((vpData->SvmFeatures & CPUID_SVM_FEATURES_EDX_VMCB_CLEAN) != 0)
    {
        vmcb->VmcbCleanBits = SVM_VMCB_CLEAN_ALL;
    }

    //
    // Count the exit
    //
    index = ShvSvmExitIndex(guestContext.ExitReason);
    vpData->ExitCounts[index]++;

    //
    // Dispatch the exit
    //
    ShvArch->HandleExit(&guestContext);

    //
    // Sync RAX back to the VMCB for the next VMRUN
    //
    vmcb->Rax = Context->Rax;

    //
    // On unload leave SVM and return to the guest, with its FS, GS, TR, LDTR
    // and system call MSRs. #VMEXIT cleared the GIF, so set it again
    //
    if (guestContext.ExitVm != FALSE)
    {
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

        ShvOsUnprepareProcessor(vpData);

        __svm_vmload(vpData->VmcbPhysicalAddress);
        __writecr3(vmcb->Cr3);
        __svm_stgi();

        //
        // #VMEXIT restored the EFER and SPEC_CTRL from before the first VMRUN,
        // and disabled DR7, so restore the guest ones. EFER keeps SVME until
        // SVM is left. With nested paging the guest's PAT is in the VMCB, and
        // the processor's is still the one from before the first VMRUN, so
        // restore that as well, and DR6, which the VMCB holds too
        //
        __writemsr(MSR_EFER, vmcb->Efer);
        if ((vpData->SvmFeatures & CPUID_SVM_FEATURES_EDX_V_SPEC_CTRL) != 0)
        {
            __writemsr(MSR_SPEC_CTRL, vmcb->SpecCtrl);
        }
        if ((vmcb->NpEnable & SVM_NP_ENABLE) != 0)
        {
            __writemsr(MSR_PAT, vmcb->Pat);
        }
        ShvSvmLeaveRootMode();
        __writedr(6, vmcb->Dr6);
        __writedr(7, vmcb->Dr7);

        //
        // The OS runs on ASID 0 again, whose mappings the guest never
        // invalidated
        //
        ShvUtilFlushTlb();

        Context->Rsp = guestContext.GuestRsp;
        Context->Rip = (UINT64)guestContext.GuestRip;
        Context->EFlags = (UINT32)guestContext.GuestEFlags;
    }
    else
    {
        //
        // Resume into ShvSvmVmrun with the guest VMCB address in RAX, and RSP
        // back at the host frame so that the stack does not drift
        //
        Context->Rax = vpData->VmcbPhysicalAddress;
        Context->Rsp = (UINT64)Context;
        Context->Rip = (UINT64)ShvSvmVmrun;
    }

    ShvOsRestoreContext(Context);
}

INT32
ShvSvmLaunch (
    _In_ PSHV_VP_DATA VpData
    )
{
    PSHV_SVM_HOST_FRAME frame;
    CONTEXT launchContext;
    UINT64 cr3;

    //
    // #VMEXIT does not restore the host FS, GS, TR, LDTR and system call MSRs,
    // so save them for ShvSvmVmrun to load
    //
    VpData->VmcbPhysicalAddress = ShvOsGetPhysicalAddress(&VpData->Vmcb);
    VpData->HostVmcbPhysicalAddress = ShvOsGetPhysicalAddress(&VpData->HostVmcb);
    __svm_vmsave(VpData->HostVmcbPhysicalAddress);

    //
    // Start the guest with the registers captured in ShvVpLaunch. The host
    // runs on the hypervisor stack, with interrupts off
    //
    frame = (PSHV_SVM_HOST_FRAME)(VpData->ShvStackLimit + KERNEL_STACK_SIZE) - 1;
    __movsq((UINT64*)&frame->Context,
            (UINT64*)&VpData->ContextFrame,
            sizeof(CONTEXT) / sizeof(UINT64));
    frame->Context.Rax = VpData->VmcbPhysicalAddress;
    frame->Context.Rsp = (UINT64)&frame->Context;
    frame->Context.Rip = (UINT64)ShvSvmVmrun;
    frame->Context.EFlags &= ~EFLAGS_INTERRUPT_FLAG;
    frame->HostVmcbPhysicalAddress = VpData->HostVmcbPhysicalAddress;

    //
    // ShvSvmEntryHandler resumes here if the first VMRUN fails
    //
    cr3 = __readcr3();
    VpData->SvmLaunchFailed = FALSE;
    VpData->SvmLaunchContext = &launchContext;
    ShvOsCaptureContext(&launchContext);
    if (VpData->SvmLaunchFailed == FALSE)
    {
        //
        // The guest resumes in ShvVpLaunch, which checks that this
        // processor is virtualized, and makes it a flush IPI target
        //
        __writecr3(VpData->SystemDirectoryTableBase);
        ShvVpMarkVirtualized(VpData->ProcessorIndex);
        ShvOsRestoreContext(&frame->Context);
    }

    //
    // Undo the mark and leave root mode
    //
    VpData->SvmLaunchContext = NULL;
    __writecr3(cr3);
    ShvVpUnmarkVirtualized(VpData->ProcessorIndex);
    ShvSvmLeaveRootMode();
    return SHV_STATUS_NOT_AVAILABLE;
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
    vpData->FatalGuestRsp = vmcb->Rsp;

    ShvOsDebugPrint("SimpleVisor: fatal #VMEXIT on CPU %d, VP data 0x%llX\n",
                    vpData->ProcessorIndex, (UINT64)vpData);
    ShvOsDebugPrint("  ExitCode 0x%llX, ExitInfo1 0x%llX, ExitInfo2 0x%llX\n",
                    vmcb->ExitCode, vmcb->ExitInfo1, vmcb->ExitInfo2);
    ShvOsDebugPrint("  ExitIntInfo 0x%llX, RIP 0x%llX, RSP 0x%llX, RFLAGS 0x%llX\n",
                    vmcb->ExitIntInfo, vmcb->Rip, vmcb->Rsp, vmcb->Rflags);
    ShvOsDebugPrint("  CPL %u, EFER 0x%llX\n",
                    (UINT32)vmcb->Cpl, vmcb->Efer);
    ShvUtilTraceDump(32);
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
ShvSvmInjectDebugException (
    _In_ PSHV_VP_STATE VpState
    )
{
    //
    // Inject #DB. A single step raises it as a trap, past the instruction,
    // so RIP stays where it is
    //
    VpState->VpData->Vmcb.EventInj = SVM_EVENTINJ_VALID |
                                     SVM_EVENTINJ_TYPE_EXCEPTION |
                                     INTR_INFO_VECTOR_DB;
    VpState->SkipRipAdvance = TRUE;
}

VOID
ShvSvmInjectNmi (
    _In_ PSHV_VP_STATE VpState
    )
{
    //
    // Have the guest's NMI handler take an NMI, on the explicit command of
    // eulercheck. ShvSvmDeliverNmi injects it once the guest can take one,
    // as it does for the NMIs passed on to the guest. An NMI cannot be
    // injected into an NMI handler, so drop the injection then, as on VT-x
    //
    if (VpState->VpData->SvmNmiState != SHV_SVM_NMI_UNBLOCKED)
    {
        return;
    }
    _InterlockedExchange(&VpState->VpData->NmiReflectPending, 1);
    VpState->VpData->IpiCounts[ShvIpiNmisInjected]++;
}

VOID
ShvSvmFlushGuestMappings (
    _In_ PSHV_VP_DATA VpData
    )
{
    //
    // Flush the guest's mappings cached for its ASID, which is 1, on the
    // next VMRUN. Without FlushByAsid that TLB_CONTROL value is reserved and
    // fails VMRUN, so flush the entire TLB instead
    //
    VpData->Vmcb.TlbControl =
        ((VpData->SvmFeatures & CPUID_SVM_FEATURES_EDX_FLUSH_BY_ASID) != 0) ?
        SVM_TLB_CONTROL_FLUSH_ASID : SVM_TLB_CONTROL_FLUSH_ALL;
}

VOID
ShvSvmTakeHeldNmi (
    _In_ PSHV_VP_DATA VpData
    )
{
    KDESCRIPTOR hostIdtr;
    KDESCRIPTOR idleIdtr;

    //
    // #VMEXIT cleared the GIF, which holds NMIs pending. Set it for a moment
    // with the idle IDT loaded, so that a held NMI goes to its NMI gate
    // rather than to the OS's NMI handler. IF is clear in root mode, so the
    // interrupts stay pending
    //
    idleIdtr.Base = VpData->IdleIdt;
    idleIdtr.Limit = sizeof(VpData->IdleIdt) - 1;
    __sidt(&hostIdtr.Limit);
    __lidt(&idleIdtr.Limit);
    __svm_stgi();
    _mm_pause();
    __svm_clgi();
    __lidt(&hostIdtr.Limit);
}

VOID
ShvSvmReflectNmi (
    _In_ PSHV_VP_DATA VpData
    )
{
    //
    // Hold the NMI for the guest, which ShvSvmDeliverNmi injects once the
    // guest can take one, before the next VMRUN. Root mode takes NMIs only
    // while it sets the GIF in an exit handler, so this works from the NMI
    // gate as well. NMIs coalesce while one is pending, as they do in
    // hardware
    //
    _InterlockedExchange(&VpData->NmiReflectPending, 1);
    VpData->IpiCounts[ShvIpiNmisReflected]++;
}

static
VOID
ShvSvmReinjectEvent (
    _In_ PSVM_VMCB Vmcb
    )
{
    UINT64 event;
    UINT32 type;
    UINT32 vector;

    //
    // An exit during the delivery of an event, such as an NMI arriving while
    // the guest takes an interrupt, leaves the event undelivered in
    // EXITINTINFO, which has the EVENTINJ format. Deliver it on the next
    // VMRUN. A software interrupt, and #BP and #OF, which INT3 and INTO
    // raise, would return to the instruction that SimpleVisor cannot decode
    // the length of, but RIP still points at it, so it runs again instead
    //
    event = Vmcb->ExitIntInfo;
    if ((event & SVM_EVENTINJ_VALID) == 0)
    {
        return;
    }

    type = (UINT32)event & SVM_EVENTINJ_TYPE_MASK;
    vector = (UINT32)event & SVM_EVENTINJ_VECTOR_MASK;
    if ((type == SVM_EVENTINJ_TYPE_SOFT_INTR) ||
        ((type == SVM_EVENTINJ_TYPE_EXCEPTION) &&
         ((vector == INTR_INFO_VECTOR_BP) || (vector == INTR_INFO_VECTOR_OF))))
    {
        return;
    }
    Vmcb->EventInj = event;
}

VOID
ShvSvmHandleNmi (
    _In_ PSHV_VP_STATE VpState
    )
{
    //
    // The NMI intercept leaves the NMI pending until the GIF is set, so take
    // it through the NMI gate of the idle IDT, after delivering the event
    // the NMI interrupted. The gate flushes for SimpleVisor's flush IPIs,
    // which the guest never sees. Any other NMI is the guest's, such as the
    // one Windows freezes the processors with for the debugger when one of
    // them does not take the freeze IPI in time, so the gate passes it on.
    // The exit is not tied to an instruction, so RIP stays where it is
    //
    VpState->SkipRipAdvance = TRUE;
    ShvSvmReinjectEvent(&VpState->VpData->Vmcb);
    ShvSvmTakeHeldNmi(VpState->VpData);
}

VOID
ShvSvmHandleIret (
    _In_ PSHV_VP_STATE VpState
    )
{
    PSHV_VP_DATA vpData = VpState->VpData;

    //
    // The IRET intercept comes before the IRET runs. Resuming at it runs the
    // IRET, as ShvSvmDeliverNmi turns the intercept off. Inside the guest's
    // NMI handler NMIs stay blocked until that IRET completes, which the
    // first exit past it tells. Outside of it, the guest can take an NMI
    // here
    //
    VpState->SkipRipAdvance = TRUE;
    if (vpData->SvmNmiState == SHV_SVM_NMI_IN_HANDLER)
    {
        vpData->SvmNmiState = SHV_SVM_NMI_IRET_RUNNING;
        vpData->SvmNmiIretRip = vpData->Vmcb.Rip;
    }
}

VOID
ShvSvmHandleDebugException (
    _In_ PSHV_VP_STATE VpState
    )
{
    PSHV_VP_DATA vpData = VpState->VpData;
    PSVM_VMCB vmcb = &vpData->Vmcb;
    UINT64 stepDr6;

    //
    // #DB is intercepted only while single-stepping to an NMI window, which
    // this exit ended (see ShvSvmEndNmiStep). The single step started with
    // BS clear (see ShvSvmDeliverNmi). One that set no other DR6 bit is
    // SimpleVisor's own, unless the guest single-steps as well, so give the
    // guest back its DR6. Otherwise the guest takes the #DB, with the BS it
    // had. A single step is a trap, past the instruction, so RIP stays where
    // it is
    //
    stepDr6 = vpData->SvmNmiStepDr6 & ~DR6_BS;
    if ((vpData->SvmNmiStepGuestTf == FALSE) &&
        ((vmcb->Dr6 & ~stepDr6) == DR6_BS))
    {
        vmcb->Dr6 = vpData->SvmNmiStepDr6;
        vmcb->VmcbCleanBits &= ~SVM_VMCB_CLEAN_DRX;
        VpState->SkipRipAdvance = TRUE;
        return;
    }
    vmcb->Dr6 |= vpData->SvmNmiStepDr6 & DR6_BS;
    vmcb->VmcbCleanBits &= ~SVM_VMCB_CLEAN_DRX;
    ShvSvmInjectDebugException(VpState);
}

static
VOID
ShvSvmEndNmiStep (
    _In_ PSHV_VP_STATE VpState
    )
{
    PSHV_VP_DATA vpData = VpState->VpData;
    PSVM_VMCB vmcb = &vpData->Vmcb;

    //
    // Any exit ends the single step to an NMI window, whether or not the
    // step completed, as ShvSvmDeliverNmi looks at what holds the NMI off
    // again before the next VMRUN, and turns the #DB intercept off unless
    // it steps again. Clear the TF it set, unless the step ran the IRET of
    // the guest's NMI handler, which loaded RFLAGS from the guest's stack.
    // Give the guest back the BS it cleared, unless this is the #DB, whose
    // handler looks at BS first
    //
    if ((vmcb->InterceptException & SVM_INTERCEPT_EXCEPTION_DB) == 0)
    {
        return;
    }

    if ((vpData->SvmNmiStepGuestTf == FALSE) &&
        ((vpData->SvmNmiState != SHV_SVM_NMI_IRET_RUNNING) ||
         (vmcb->Rip == vpData->SvmNmiIretRip)))
    {
        vmcb->Rflags &= ~EFLAGS_TRAP_FLAG;
    }
    VpState->GuestEFlags = vmcb->Rflags;

    if ((VpState->ExitReason != VMEXIT_EXCEPTION_DB) &&
        ((vpData->SvmNmiStepDr6 & DR6_BS) != 0))
    {
        vmcb->Dr6 |= DR6_BS;
        vmcb->VmcbCleanBits &= ~SVM_VMCB_CLEAN_DRX;
    }
}

static
VOID
ShvSvmDeliverNmi (
    _In_ PSHV_VP_STATE VpState
    )
{
    PSHV_VP_DATA vpData = VpState->VpData;
    PSVM_VMCB vmcb = &vpData->Vmcb;
    UINT32 interceptMisc1;
    UINT32 interceptException;
    UINT8 pending;
    UINT8 eventPending;

    //
    // SVM has no NMI window exit, and EVENTINJ delivers an NMI even inside
    // the guest's NMI handler, and in the STI and MOV SS shadow. Without
    // vNMI the VMCB does not show whether the guest blocks NMIs either, but
    // the guest takes only the NMIs SimpleVisor injects, so it blocks them
    // from the injection until the IRET of its NMI handler completes. The
    // first exit past the intercepted IRET tells
    //
    // vNMI (CPUID 8000_000A EDX bit 25, Zen 4 and later) would be simpler,
    // as the processor tracks the guest's NMI blocking and delivers a pending
    // NMI once the guest can take it. It is not used, as whether nested
    // Hyper-V and OpenVMM expose it is unknown, and this works without it
    //
    if ((vpData->SvmNmiState == SHV_SVM_NMI_IRET_RUNNING) &&
        (vmcb->Rip != vpData->SvmNmiIretRip))
    {
        vpData->SvmNmiState = SHV_SVM_NMI_UNBLOCKED;
    }

    //
    // Inject the pending NMI once the guest can take one and no other event
    // is to be delivered
    //
    pending = (vpData->NmiReflectPending != 0);
    eventPending = ((vmcb->EventInj & SVM_EVENTINJ_VALID) != 0);
    if ((pending != FALSE) &&
        (vpData->SvmNmiState == SHV_SVM_NMI_UNBLOCKED) &&
        ((vmcb->InterruptShadow & SVM_INTERRUPT_SHADOW) == 0) &&
        (eventPending == FALSE))
    {
        _InterlockedExchange(&vpData->NmiReflectPending, 0);
        vmcb->EventInj = SVM_EVENTINJ_VALID |
                         SVM_EVENTINJ_TYPE_NMI |
                         INTR_INFO_VECTOR_NMI;
        vpData->SvmNmiState = SHV_SVM_NMI_IN_HANDLER;
        pending = FALSE;
    }

    //
    // Otherwise exit once the guest can take it. Inside its NMI handler that
    // is past the handler's IRET, so intercept the IRET, and then single-step
    // over it, as NMIs stay blocked until it completes. In the shadow, single-
    // step the instruction it covers. With another event to deliver, intercept
    // the IRET of that event's handler instead, as a single step would leave
    // TF set in the frame the event pushes. Single-step with BS clear, as
    // the guest may have left it set from a single step of its own, so that
    // the #DB tells whether the step completed
    //
    interceptMisc1 = vmcb->InterceptMisc1 & ~SVM_INTERCEPT_IRET;
    interceptException = vmcb->InterceptException & ~SVM_INTERCEPT_EXCEPTION_DB;
    if ((vpData->SvmNmiState == SHV_SVM_NMI_IN_HANDLER) ||
        ((pending != FALSE) &&
         (vpData->SvmNmiState == SHV_SVM_NMI_UNBLOCKED) &&
         (eventPending != FALSE)))
    {
        interceptMisc1 |= SVM_INTERCEPT_IRET;
    }
    else if ((pending != FALSE) && (eventPending == FALSE))
    {
        vpData->SvmNmiStepGuestTf = ((vmcb->Rflags & EFLAGS_TRAP_FLAG) != 0);
        vpData->SvmNmiStepDr6 = vmcb->Dr6;
        if ((vmcb->Dr6 & DR6_BS) != 0)
        {
            vmcb->Dr6 &= ~DR6_BS;
            vmcb->VmcbCleanBits &= ~SVM_VMCB_CLEAN_DRX;
        }
        vmcb->Rflags |= EFLAGS_TRAP_FLAG;
        interceptException |= SVM_INTERCEPT_EXCEPTION_DB;
    }

    if ((interceptMisc1 != vmcb->InterceptMisc1) ||
        (interceptException != vmcb->InterceptException))
    {
        vmcb->InterceptMisc1 = interceptMisc1;
        vmcb->InterceptException = interceptException;
        vmcb->VmcbCleanBits &= ~SVM_VMCB_CLEAN_I;
    }
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
    if ((VpState->VpRegs->Rax == SHV_CPUID_LEAF) &&
        (VpState->VpRegs->Rcx == SHV_CPUID_UNLOAD) &&
        (VpState->VpData->Vmcb.Cpl == DPL_SYSTEM))
    {
        VpState->ExitVm = TRUE;
        return;
    }

    //
    // Magic CPUID command to inject an NMI into this processor, which
    // eulercheck uses to exercise the NMI path. The NMI delivers once the
    // CPUID completes and the guest can take it
    //
    if (((UINT32)VpState->VpRegs->Rax == SHV_CPUID_LEAF) &&
        ((UINT32)VpState->VpRegs->Rcx == SHV_CPUID_NMI))
    {
        ShvSvmInjectNmi(VpState);
        return;
    }

    //
    // Magic CPUID sequence to detect SimpleVisor. EBX reports whether this
    // processor's local APIC is in x2APIC mode, and whether the flush IPIs
    // reach this processor
    //
    if (((UINT32)VpState->VpRegs->Rax == SHV_CPUID_LEAF) &&
        ((UINT32)VpState->VpRegs->Rcx == SHV_CPUID_DETECT))
    {
        VpState->VpRegs->Rax = SHV_CPUID_SIGNATURE;
        VpState->VpRegs->Rbx = ((VpState->VpData->X2ApicEnabled != FALSE) ? SHV_CPUID_DETECT_X2APIC : 0) |
                               ((VpState->VpData->FlushIpiIcr != 0) ? SHV_CPUID_DETECT_FLUSH_IPIS : 0);
        VpState->VpRegs->Rcx = 0;
        VpState->VpRegs->Rdx = 0;
        return;
    }

    //
    // Magic CPUID sequence to read this processor's #VMEXIT counters, one
    // ShvSvmExitIndex per call, given in the low 16 bits of ECX
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
    // Likewise for the TLB flush hypercalls forwarded to Hyper-V
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
    // Issue the CPUID to the logical processor
    //
    __cpuidex(cpu_info, (INT32)VpState->VpRegs->Rax, (INT32)VpState->VpRegs->Rcx);

    if ((UINT32)VpState->VpRegs->Rax == 1)
    {
        //
        // Set the Hypervisor Present-bit
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
        // SimpleVisor, rather than those of the guest on ASID 1. The
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
        // Return the SimpleVisor vendor. When nested pass the underlying
        // hypervisor's through
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
        // Return the SimpleVisor interface identifier
        //
        cpu_info[0] = SHV_CPUID_SIGNATURE;
    }

    //
    // CPUID zero-extends the results into the 64-bit registers
    //
    VpState->VpRegs->Rax = (UINT32)cpu_info[0];
    VpState->VpRegs->Rbx = (UINT32)cpu_info[1];
    VpState->VpRegs->Rcx = (UINT32)cpu_info[2];
    VpState->VpRegs->Rdx = (UINT32)cpu_info[3];
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
    // guest. Raise #GP for reserved bits and for changing LME with paging on,
    // as the processor would, rather than fail the next VMRUN. LMA is read-only
    // and writes to it are ignored
    //
    if (msr == MSR_EFER)
    {
        if (vmcb->ExitInfo1 != 0)
        {
            value = VpState->VpRegs->Rdx << 32 | (UINT32)VpState->VpRegs->Rax;
            if (((value & ~VpState->VpData->SvmEferValidBits) != 0) ||
                (((vmcb->Cr0 & CR0_PG) != 0) &&
                 (((value ^ vmcb->Efer) & EFER_LME) != 0)))
            {
                ShvSvmInjectGeneralProtection(VpState);
                return;
            }
            vmcb->Efer = (value & ~EFER_LMA) | (vmcb->Efer & EFER_LMA) | EFER_SVME;
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
    // SVM stays in CPUID, but looks disabled by the firmware, with VM_CR.SVMDIS
    // and VM_CR.LOCK set, so VM_CR is read-only. VM_HSAVE_PA has no effect
    // without SVM, so the guest gets a copy of its own
    //
    if (msr == MSR_VM_CR)
    {
        if (vmcb->ExitInfo1 != 0)
        {
            ShvSvmInjectGeneralProtection(VpState);
        }
        else
        {
            value = __readmsr(MSR_VM_CR) | VM_CR_SVMDIS | VM_CR_LOCK;
            VpState->VpRegs->Rax = (UINT32)value;
            VpState->VpRegs->Rdx = value >> 32;
        }
        return;
    }
    if (msr == MSR_VM_HSAVE_PA)
    {
        if (vmcb->ExitInfo1 != 0)
        {
            VpState->VpData->SvmGuestHostSaveArea =
                VpState->VpRegs->Rdx << 32 | (UINT32)VpState->VpRegs->Rax;
        }
        else
        {
            value = VpState->VpData->SvmGuestHostSaveArea;
            VpState->VpRegs->Rax = (UINT32)value;
            VpState->VpRegs->Rdx = value >> 32;
        }
        return;
    }

    //
    // Count the IPIs the guest sends through the ICR and the self IPI
    // register, and pass the writes on. The ICR does not say what the IPI is
    // for, as the OS carries that in its own per-processor IPI mailbox, so
    // SimpleVisor cannot tell the TLB flushes from the other IPIs
    //
    if (vmcb->ExitInfo1 != 0)
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

    //
    // The others are the ICR and self IPI writes counted above, and those
    // outside the MSR permission map ranges, such as the synthetic MSRs of
    // Hyper-V and the Scalable MCA MSRs. Access them from the host as on
    // VT-x, and give the guest the #GP the processor or the hypervisor we
    // are nested under raises
    //
    if (vmcb->ExitInfo1 != 0)
    {
        value = VpState->VpRegs->Rdx << 32 | (UINT32)VpState->VpRegs->Rax;
        if (ShvUtilAccessMsr(VpState->VpData, msr, TRUE, &value) == FALSE)
        {
            ShvSvmInjectGeneralProtection(VpState);
        }
    }
    else
    {
        if (ShvUtilAccessMsr(VpState->VpData, msr, FALSE, &value) == FALSE)
        {
            ShvSvmInjectGeneralProtection(VpState);
            return;
        }
        VpState->VpRegs->Rax = (UINT32)value;
        VpState->VpRegs->Rdx = value >> 32;
    }
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
ShvSvmHandleSwitchAddressSpace (
    _In_ PSHV_VP_STATE VpState
    )
{
    UINT64 addressSpace;
    PSVM_VMCB vmcb = &VpState->VpData->Vmcb;

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
    vmcb->Cr3 = addressSpace;
    vmcb->VmcbCleanBits &= ~SVM_VMCB_CLEAN_CRX;
    ShvSvmFlushGuestMappings(VpState->VpData);
    VpState->VpRegs->Rax = HYPERV_STATUS_SUCCESS;
}

VOID
ShvSvmHandleVmmcall (
    _In_ PSHV_VP_STATE VpState
    )
{
    UINT8 flush;

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

    //
    // The switch hypercall changes the guest's address space, which a
    // forwarded hypercall does not, so handle it separately
    //
    if ((VpState->VpRegs->Rcx & HYPERV_HYPERCALL_CODE_MASK) ==
        HvCallSwitchVirtualAddressSpace)
    {
        ShvSvmHandleSwitchAddressSpace(VpState);
        return;
    }

    flush = ShvUtilCountHypercall(VpState->VpData, VpState->VpRegs->Rcx);
    ShvSvmHypercall(VpState->VpRegs);
    if (flush != FALSE)
    {
        ShvVpFlushGuestMappings(VpState->VpData, ShvSvmOwnHypercall);
    }
}

static
UINT8
ShvSvmNmiWaits (
    _In_ PSHV_VP_DATA VpData
    )
{
    //
    // Tell whether an NMI waits for a guest that can take it, which
    // ShvSvmDeliverNmi then injects on the next VMRUN
    //
    return (VpData->NmiReflectPending != 0) &&
           (VpData->SvmNmiState == SHV_SVM_NMI_UNBLOCKED);
}

VOID
ShvSvmHandleHlt (
    _In_ PSHV_VP_STATE VpState
    )
{
    PSVM_VMCB vmcb = &VpState->VpData->Vmcb;
    KDESCRIPTOR hostIdtr;
    KDESCRIPTOR idleIdtr;
    UINT32 vector;

    //
    // Complete the guest's HLT by halting in the host until an interrupt
    // comes in, as VT-x does in root mode. An HLT with interrupts disabled
    // only waits for an NMI, so just resume then. Single-stepping the HLT
    // resumes too, as EVENTINJ is taken by its #DB, and so does an NMI that
    // waits for the guest, which wakes the HLT once injected.
    //
    // #VMEXIT cleared the GIF, which holds interrupts and NMIs pending, so set
    // it while halted, with the idle IDT loaded around it as in
    // ShvSvmTakeHeldNmi. A flush IPI that came along with the exit is taken
    // as soon as the GIF is set, and through the OS's IDT its NMI handler
    // would take it for a hardware failure. One of the guest's NMIs that came
    // along is taken there too, and waits for the guest, so resume then
    // rather than halt. Inject the interrupt into the guest on VMRUN, which
    // ends the interrupt shadow of sti; hlt with the HLT. Without one the
    // guest just resumes
    //
    if (((vmcb->Rflags & EFLAGS_INTERRUPT_FLAG) == 0) ||
        ((vmcb->Rflags & EFLAGS_TRAP_FLAG) != 0) ||
        (ShvSvmNmiWaits(VpState->VpData) != FALSE))
    {
        return;
    }

    idleIdtr.Base = VpState->VpData->IdleIdt;
    idleIdtr.Limit = sizeof(VpState->VpData->IdleIdt) - 1;
    __sidt(&hostIdtr.Limit);
    __lidt(&idleIdtr.Limit);
    __svm_stgi();
    vector = 0;
    if (ShvSvmNmiWaits(VpState->VpData) == FALSE)
    {
        vector = ShvUtilIdle(VpState->VpData);
    }
    __svm_clgi();
    __lidt(&hostIdtr.Limit);

    if (vector != 0)
    {
        vmcb->EventInj = SVM_EVENTINJ_VALID |
                         SVM_EVENTINJ_TYPE_INTR |
                         vector;
    }
}

VOID
ShvSvmHandleExit (
    _In_ PSHV_VP_STATE VpState
    )
{
    PSVM_VMCB vmcb = &VpState->VpData->Vmcb;

    //
    // Unlike VT-x, nested Hyper-V does not force MONITOR and MWAIT exits, and
    // there is no activity state to fail VM entry on. HLT exits so that the
    // guest idles in the host, as on VT-x. NMIs exit through their intercept,
    // for the flush IPIs and to pass any other on to the guest, and IRET and
    // #DB to tell when the guest can take it. The guest takes its interrupts
    // directly
    //
    ShvSvmEndNmiStep(VpState);
    switch (VpState->ExitReason)
    {
    case VMEXIT_NMI:
        ShvSvmHandleNmi(VpState);
        break;
    case VMEXIT_IRET:
        ShvSvmHandleIret(VpState);
        break;
    case VMEXIT_EXCEPTION_DB:
        ShvSvmHandleDebugException(VpState);
        break;
    case VMEXIT_CPUID:
        ShvSvmHandleCpuid(VpState);
        break;
    case VMEXIT_INVD:
        //
        // Write back the caches rather than drop them, as on VT-x
        //
        __wbinvd();
        break;
    case VMEXIT_HLT:
        ShvSvmHandleHlt(VpState);
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
    // handler injected a fault. Like any instruction, it ends the STI and
    // MOV SS shadow, and single-steps with RFLAGS.TF, unless DEBUGCTL.BTF
    // limits that to branches. Without LBR virtualization the guest DEBUGCTL
    // is the processor's
    //
    if (VpState->SkipRipAdvance == FALSE)
    {
        VpState->GuestRip = vmcb->NextRip;
        vmcb->Rip = VpState->GuestRip;
        vmcb->InterruptShadow &= ~SVM_INTERRUPT_SHADOW;

        if (((vmcb->Rflags & EFLAGS_TRAP_FLAG) != 0) &&
            ((__readmsr(MSR_DEBUG_CTL) & DEBUG_CTL_BTF) == 0))
        {
            vmcb->Dr6 |= DR6_BS;
            vmcb->VmcbCleanBits &= ~SVM_VMCB_CLEAN_DRX;
            ShvSvmInjectDebugException(VpState);
        }
    }

    //
    // Pass a pending NMI on to the guest, now that RIP, the shadow and the
    // event to deliver are settled, unless the guest leaves SVM
    //
    if (VpState->ExitVm == FALSE)
    {
        ShvSvmDeliverNmi(VpState);
    }
}

//
// The SVM backend
//
const SHV_ARCH ShvSvmArch =
{
    ShvSvmProbe,
    ShvSvmBuildPaging,
    ShvSvmEnterRootModeOnVp,
    ShvSvmSetupVmcbForVp,
    ShvSvmLaunch,
    ShvSvmHandleExit,
    ShvSvmFlushGuestMappings,
    ShvSvmTakeHeldNmi,
    ShvSvmReflectNmi,
};
