/*++

Copyright (c) Alex Ionescu.  All rights reserved.

Module Name:

    shvvp.c

Abstract:

    This module implements Virtual Processor (VP) management for the Simple Hyper Visor.

Author:

    Alex Ionescu (@aionescu) 16-Mar-2016 - Initial version

Environment:

    Kernel mode only, IRQL DISPATCH_LEVEL.

--*/

#include "shv.h"

//
// One bit per logical processor that SimpleVisor runs on. Only the backend
// sets it, once the guest is about to run: ShvVpRestoreAfterLaunch after a
// successful VMLAUNCH, or ShvSvmLaunch right before the first VMRUN. Unlike
// EFLAGS.AC, which a kernel using SMAP sets on its own, the OS cannot set it
//
static volatile long* ShvVpVirtualizedBitmap;
static UINT32 ShvVpVirtualizedBitmapCount;

//
// The root of each processor's second level identity map, by processor
// number, or 0 if SimpleVisor does not run there. Each processor has its own,
// and the guest's cached mappings are tagged with it
//
static volatile UINT64* ShvVpSecondLevelRoots;

INT32
ShvVpAllocateTracking (
    _In_ UINT32 CpuCount
    )
{
    size_t size;

    //
    // One bit per logical processor in whole longs for the bit intrinsics
    //
    size = ((CpuCount + 31) / 32) * sizeof(long);
    ShvVpVirtualizedBitmap = ShvOsAllocateContigousAlignedMemory(size);
    if (ShvVpVirtualizedBitmap == NULL)
    {
        return SHV_STATUS_NO_RESOURCES;
    }
    __stosb((UINT8*)ShvVpVirtualizedBitmap, 0, size);

    ShvVpSecondLevelRoots = ShvOsAllocateContigousAlignedMemory(CpuCount * sizeof(UINT64));
    if (ShvVpSecondLevelRoots == NULL)
    {
        ShvOsFreeContiguousAlignedMemory((VOID*)ShvVpVirtualizedBitmap, size);
        ShvVpVirtualizedBitmap = NULL;
        return SHV_STATUS_NO_RESOURCES;
    }
    __stosq((UINT64*)ShvVpSecondLevelRoots, 0, CpuCount);
    ShvVpVirtualizedBitmapCount = CpuCount;
    return SHV_STATUS_SUCCESS;
}

VOID
ShvVpFreeTracking (
    VOID
    )
{
    if (ShvVpVirtualizedBitmap != NULL)
    {
        ShvOsFreeContiguousAlignedMemory((VOID*)ShvVpVirtualizedBitmap,
                                         ((ShvVpVirtualizedBitmapCount + 31) / 32) * sizeof(long));
        ShvOsFreeContiguousAlignedMemory((VOID*)ShvVpSecondLevelRoots,
                                         ShvVpVirtualizedBitmapCount * sizeof(UINT64));
        ShvVpVirtualizedBitmap = NULL;
        ShvVpSecondLevelRoots = NULL;
        ShvVpVirtualizedBitmapCount = 0;
    }
}

UINT8
ShvIsOurHypervisorPresent (
    VOID
    )
{
    INT32 cpu;

    //
    // SimpleVisor runs on this logical processor if its bit is set
    //
    cpu = ShvOsGetCurrentProcessorNumber();
    if ((ShvVpVirtualizedBitmap == NULL) ||
        (cpu < 0) ||
        ((UINT32)cpu >= ShvVpVirtualizedBitmapCount))
    {
        return FALSE;
    }
    return _bittest((long*)ShvVpVirtualizedBitmap, cpu) != 0;
}

VOID
ShvVpMarkVirtualized (
    _In_ UINT32 ProcessorIndex
    )
{
    _interlockedbittestandset(ShvVpVirtualizedBitmap, ProcessorIndex);
}

VOID
ShvVpUnmarkVirtualized (
    _In_ UINT32 ProcessorIndex
    )
{
    _interlockedbittestandreset(ShvVpVirtualizedBitmap, ProcessorIndex);
}

VOID
ShvVpSetSecondLevelRoot (
    _In_ UINT32 ProcessorIndex,
    _In_ UINT64 Root
    )
{
    ShvVpSecondLevelRoots[ProcessorIndex] = Root;
}

VOID
ShvVpFlushGuestMappings (
    _In_ PSHV_VP_DATA VpData,
    _In_ PSHV_HYPERCALL Hypercall
    )
{
    UINT32 i;
    UINT64 root;

    //
    // Nested under Hyper-V, the guest flushes the TLB of other processors with
    // hypercalls, which SimpleVisor forwards from root mode. Hyper-V then
    // flushes the mappings of SimpleVisor, not those the guest cached under
    // VPID or ASID 1. Flush those as well, on every processor, by flushing all
    // mappings cached for each processor's second level identity map. That is
    // more than the guest asked for, but its TLB refills from the same map
    //
    if (VpData->HyperVGuestMappingFlush == FALSE)
    {
        return;
    }

    for (i = 0; i < ShvVpVirtualizedBitmapCount; i++)
    {
        root = ShvVpSecondLevelRoots[i];
        if (root != 0)
        {
            VpData->GuestMappingFlushInput[0] = root;
            VpData->GuestMappingFlushInput[1] = 0;
            Hypercall(HvCallFlushGuestPhysicalAddressSpace,
                      VpData->GuestMappingFlushInputPhysicalAddress,
                      0);
        }
    }
}

VOID
ShvCaptureSpecialRegisters (
    _In_ PSHV_SPECIAL_REGISTERS SpecialRegisters
    )
{
    //
    // Use compiler intrinsics to get the data we need
    //
    SpecialRegisters->Cr0 = __readcr0();
    SpecialRegisters->Cr3 = __readcr3();
    SpecialRegisters->Cr4 = __readcr4();
    SpecialRegisters->DebugControl = __readmsr(MSR_DEBUG_CTL);
    SpecialRegisters->MsrGsBase = __readmsr(MSR_GS_BASE);
    SpecialRegisters->KernelDr7 = __readdr(7);
    _sgdt(&SpecialRegisters->Gdtr.Limit);
    __sidt(&SpecialRegisters->Idtr.Limit);

    //
    // Use OS-specific functions to get these two
    //
    _str(&SpecialRegisters->Tr);
    _sldt(&SpecialRegisters->Ldtr);
}

VOID
ShvVpDetectUnderlyingHypervisor (
    _In_ PSHV_VP_DATA VpData
    )
{
    INT32 cpuInfo[4];

    //
    // SimpleVisor is not running yet, so CPUID reaches the processor or the
    // hypervisor we are nested under, which sets the Hypervisor Present-bit
    //
    __cpuid(cpuInfo, 1);
    VpData->UnderlyingHypervisor = (cpuInfo[2] & HYPERV_HYPERVISOR_PRESENT_BIT) != 0;
    if (VpData->UnderlyingHypervisor == FALSE)
    {
        return;
    }

    //
    // Check for the "VMwareVMware" and "Microsoft Hv" vendor signatures
    //
    __cpuid(cpuInfo, HYPERV_CPUID_VENDOR_AND_MAX_FUNCTIONS);
    VpData->UnderlyingHypervisorIsVmware = (cpuInfo[1] == 'awMV') &&
                                           (cpuInfo[2] == 'MVer') &&
                                           (cpuInfo[3] == 'eraw');
    VpData->UnderlyingHypervisorIsHyperV = (cpuInfo[1] == 'rciM') &&
                                           (cpuInfo[2] == 'foso') &&
                                           (cpuInfo[3] == 'vH t');

    //
    // Check whether Hyper-V can flush the guest's cached mappings on every
    // processor, and keep the physical address of that hypercall's input
    //
    if ((VpData->UnderlyingHypervisorIsHyperV != FALSE) &&
        ((UINT32)cpuInfo[0] >= HYPERV_CPUID_NESTED_FEATURES))
    {
        __cpuid(cpuInfo, HYPERV_CPUID_NESTED_FEATURES);
        VpData->HyperVGuestMappingFlush =
            (cpuInfo[0] & HYPERV_NESTED_GUEST_MAPPING_FLUSH) != 0;
        VpData->GuestMappingFlushInputPhysicalAddress =
            ShvOsGetPhysicalAddress(VpData->GuestMappingFlushInput);
    }
}

INT32
ShvVpLaunch (
    _In_ PSHV_VP_DATA VpData
    )
{
    INT32 status;
    //
    // Being nested changes how CPUID and the identity map are handled
    //
    ShvVpDetectUnderlyingHypervisor(VpData);

    //
    // Initialize all the MTRR-related MSRs by reading their value and build
    // range structures to describe their settings
    //
    ShvUtilMtrrInitialize(VpData);

    //
    // Build the identity map of physical memory
    //
    ShvArch->BuildPaging(VpData);

    //
    // Attempt to enter root mode on this processor.
    //
    if (ShvArch->Enable(VpData) == FALSE)
    {
        //
        // We could not enter root mode
        //
        return SHV_STATUS_NOT_AVAILABLE;
    }

    //
    // Initialize the guest and host state.
    //
    ShvArch->Setup(VpData);

    //
    // Launch the guest. This will cause the processor to jump to
    // ShvVpRestoreAfterLaunch on success, or return back to the caller on
    // failure, when the second level identity map is no longer used
    //
    status = ShvArch->Run(VpData);
    ShvVpSetSecondLevelRoot(VpData->ProcessorIndex, 0);
    return status;
}

DECLSPEC_NORETURN
VOID
ShvVpRestoreAfterLaunch (
    VOID
    )
{
    PSHV_VP_DATA vpData;

    //
    // Get the per-processor data. This routine temporarily executes on the
    // same stack as the hypervisor (using no real stack space except the home
    // registers), so we can retrieve the VP the same way the hypervisor does.
    // Account for the 8 byte bias that stands in for a return address (see
    // ShvVmxSetupVmcsForVp)
    //
    vpData = (PSHV_VP_DATA)((uintptr_t)_AddressOfReturnAddress() +
                            sizeof(UINT64) +
                            sizeof(CONTEXT) -
                            KERNEL_STACK_SIZE);

    //
    // Mark this processor as virtualized, which tells ShvVpInitialize that the
    // launch happened. The index comes from the VP data as no stack is used
    //
    _interlockedbittestandset(ShvVpVirtualizedBitmap, vpData->ProcessorIndex);

    //
    // And finally, restore the context, so that all register and stack
    // state is finally restored.
    //
    ShvOsRestoreContext(&vpData->ContextFrame);
}

INT32
ShvVpInitialize (
    _In_ PSHV_VP_DATA Data
    )
{
    INT32 status;
    INT32 cpu;

    //
    // Find the bit tracking this processor, which ShvVpRestoreAfterLaunch sets
    //
    cpu = ShvOsGetCurrentProcessorNumber();
    if ((ShvVpVirtualizedBitmap == NULL) ||
        (cpu < 0) ||
        ((UINT32)cpu >= ShvVpVirtualizedBitmapCount))
    {
        return SHV_STATUS_NOT_AVAILABLE;
    }
    Data->ProcessorIndex = (UINT32)cpu;

    //
    // Prepare any OS-specific CPU data
    //
    status = ShvOsPrepareProcessor(Data);
    if (status != SHV_STATUS_SUCCESS)
    {
        return status;
    }

    // Read the special control registers for this processor
    // Note: KeSaveStateForHibernate(&Data->HostState) can be used as a Windows
    // specific undocumented function that can also get this data.
    //
    ShvCaptureSpecialRegisters(&Data->SpecialRegisters);

    //
    // Then, capture the entire register state. We will need this, as once we
    // launch the VM, it will begin execution at the defined guest instruction
    // pointer, which we set to ShvVpRestoreAfterLaunch, with the registers set
    // to whatever value they were deep inside the VMCS/VMX initialization code.
    // That function marks this processor as virtualized and returns here with
    // our registers restored by RtlRestoreContext.
    //
    ShvOsCaptureContext(&Data->ContextFrame);
    if (ShvIsOurHypervisorPresent() == FALSE)
    {
        //
        // If this processor is not marked as virtualized, we have not launched
        // the VM yet. Attempt to launch it on this processor.
        //
        status = ShvVpLaunch(Data);
    }

    //
    // If we got here, the hypervisor is running :-)
    //
    return status;
}

VOID
ShvVpUnloadCallback (
    _In_ PSHV_CALLBACK_CONTEXT Context
    )
{
    INT32 cpuInfo[4];
    PSHV_VP_DATA vpData;
    UINT32 i;

    //
    // Skip processors SimpleVisor does not run on, such as one that failed to
    // load
    //
    if (ShvIsOurHypervisorPresent() == FALSE)
    {
        return;
    }

    //
    // Send the magic shutdown instruction sequence. It will return in EAX:EBX
    // the VP data for the current CPU. Add up its exit and hypercall counters
    // and free it.
    //
    cpuInfo[0] = cpuInfo[1] = 0;
    __cpuidex(cpuInfo, 0x41414141, 0x42424242);

    //
    // If SimpleVisor is disabled for some reason, CPUID will return the values
    // of the highest valid CPUID. We use a magic value to make sure we really
    // are loaded and returned something valid.
    //
    if (cpuInfo[2] == 0x43434343)
    {
        __analysis_assume((cpuInfo[0] != 0) && (cpuInfo[1] != 0));
        vpData = (PSHV_VP_DATA)((UINT64)cpuInfo[0] << 32 | (UINT32)cpuInfo[1]);
        _interlockedbittestandreset(ShvVpVirtualizedBitmap, vpData->ProcessorIndex);
        ShvVpSetSecondLevelRoot(vpData->ProcessorIndex, 0);
        for (i = 0; i < SHV_EXIT_REASON_COUNT; i++)
        {
            _InterlockedExchangeAdd64(&Context->ExitCounts[i], (INT64)vpData->ExitCounts[i]);
        }
        for (i = 0; i < ShvTlbHypercallCount; i++)
        {
            _InterlockedExchangeAdd64(&Context->TlbHypercallCounts[i], (INT64)vpData->TlbHypercallCounts[i]);
        }
        ShvOsFreeContiguousAlignedMemory(vpData, sizeof(*vpData));
    }
}

PSHV_VP_DATA
ShvVpAllocateData (
    _In_ UINT32 CpuCount
    )
{
    PSHV_VP_DATA data;
    UINT32 i;

    //
    // Allocate a contiguous chunk of RAM to back this allocation
    //
    data = ShvOsAllocateContigousAlignedMemory(sizeof(*data) * CpuCount);
    if (data != NULL)
    {
        //
        // Zero out the entire data region
        //
        __stosq((UINT64*)data, 0, (sizeof(*data) / sizeof(UINT64)) * CpuCount);

        //
        // Arm the stack canary that every VM-Exit checks
        //
        for (i = 0; i < CpuCount; i++)
        {
            data[i].StackCanary = SHV_STACK_CANARY;
        }
    }

    //
    // Return what is hopefully a valid pointer, otherwise NULL.
    //
    return data;
}

VOID
ShvVpFreeData (
    _In_ PSHV_VP_DATA Data,
    _In_ UINT32 CpuCount
    )
{
    //
    // Free the contiguous chunk of RAM
    //
    ShvOsFreeContiguousAlignedMemory(Data, sizeof(*Data) * CpuCount);
}

VOID
ShvVpLoadCallback (
    _In_ PSHV_CALLBACK_CONTEXT Context
    )
{
    PSHV_VP_DATA vpData;
    INT32 status;

    //
    // Allocate the per-VP data for this logical processor
    //
    vpData = ShvVpAllocateData(1);
    if (vpData == NULL)
    {
        status = SHV_STATUS_NO_RESOURCES;
        goto Failure;
    }

    //
    // Detect if the hardware appears to support root mode to start, and
    // capture its capabilities. No attempts are made to enable this if it is
    // lacking or disabled.
    //
    if (!ShvArch->Probe(vpData))
    {
        status = SHV_STATUS_NOT_AVAILABLE;
        goto Failure;
    }

    //
    // First, capture the value of the PML4 for the SYSTEM process, so that all
    // virtual processors, regardless of which process the current LP has
    // interrupted, can share the correct kernel address space.
    //
    vpData->SystemDirectoryTableBase = Context->Cr3;

    //
    // Initialize the virtual processor
    //
    status = ShvVpInitialize(vpData);
    if (status != SHV_STATUS_SUCCESS)
    {
        //
        // Bail out, free the allocated per-processor data
        //
        goto Failure;
    }

    //
    // Our hypervisor should now run on this LP. If not, the VM was never
    // launched here and the per-processor data can be freed.
    //
    if (ShvIsOurHypervisorPresent() == FALSE)
    {
        //
        // Free the per-processor data
        //
        status = SHV_STATUS_NOT_PRESENT;
        goto Failure;
    }

    //
    // This CPU is hyperjacked!
    //
    _InterlockedIncrement((volatile long*)&Context->InitCount);
    return;

Failure:
    //
    // Return failure
    //
    if (vpData != NULL)
    {
        ShvVpFreeData(vpData, 1);
    }
    Context->FailedCpu = ShvOsGetCurrentProcessorNumber();
    Context->FailureStatus = status;
    return;
}
