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
    __writemsr(MSR_EFER, efer | EFER_SVME);
    __writemsr(MSR_VM_HSAVE_PA, VpData->HostSaveAreaPhysicalAddress);
    return TRUE;
}

VOID
ShvSvmSetupVmcbForVp (
    _In_ PSHV_VP_DATA VpData
    )
{
    //
    // The VMCB is not set up yet, Run fails before VMRUN. With a nonzero
    // NptPml4PhysicalAddress it will set NP_ENABLE and load it into nCR3
    //
    UNREFERENCED_PARAMETER(VpData);
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

VOID
ShvSvmHandleExit (
    _In_ PSHV_VP_STATE VpState
    )
{
    UNREFERENCED_PARAMETER(VpState);

    //
    // Unreachable until ShvSvmLaunch runs the guest. Break into the debugger
    // and stop this processor
    //
    __debugbreak();
    for (;;)
    {
        __halt();
    }
}

//
// The SVM backend. The probe, the nested page tables and root mode are
// implemented, but there is no VMCB, so it fails to launch and leaves SVM
// disabled again
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
