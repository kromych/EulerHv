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
    //
    // The nested page tables are not built yet, Run fails before VMRUN
    //
    UNREFERENCED_PARAMETER(VpData);
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
    // The VMCB is not set up yet, Run fails before VMRUN
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
// The SVM backend. Only the probe and root mode are implemented, so it fails
// to launch and leaves SVM disabled again
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
