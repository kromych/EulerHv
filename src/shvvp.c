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

//
// Each virtualized processor's flush IPI state, by processor number, a cache
// line each.
//
// Icr sends the processor a flush IPI: its x2APIC ID as the ICR destination
// with the NMI delivery mode, or 0 while it cannot take one, such as before
// its guest runs. Senders count themselves in Senders while they use it, so
// that a processor leaving the guest can wait for them.
//
// A sender bumps Requested and waits for Completed to reach it. It sends the
// NMI only if it is the one to set NmiInFlight, as otherwise an NMI on its
// way covers the request: the processor clears NmiInFlight before it reads
// Requested, flushes, and copies what it read to Completed. So at most one
// flush IPI is on its way to a processor, and none merges with another. An
// NMI that finds NmiInFlight clear is not a flush IPI.
//
// VpData finds the processor from its stack pointer in the NMI gate
//
typedef struct DECLSPEC_ALIGN(64) _SHV_VP_FLUSH_TARGET
{
    volatile INT64 Icr;
    volatile INT64 Requested;
    volatile INT64 Completed;
    volatile long NmiInFlight;
    volatile long Senders;
    PSHV_VP_DATA VpData;
} SHV_VP_FLUSH_TARGET, *PSHV_VP_FLUSH_TARGET;

static PSHV_VP_FLUSH_TARGET ShvVpFlushTargets;

//
// How many times a processor leaving the guest polls for the flush IPI on
// its way. It arrives within microseconds, unless an unexpected NMI took its
// place, which clears NmiInFlight as well, so this only bounds the wait
//
#define SHV_FLUSH_IPI_DRAIN_SPINS   100000

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

    ShvVpFlushTargets = ShvOsAllocateContigousAlignedMemory(CpuCount * sizeof(SHV_VP_FLUSH_TARGET));
    if (ShvVpFlushTargets == NULL)
    {
        ShvOsFreeContiguousAlignedMemory((VOID*)ShvVpSecondLevelRoots, CpuCount * sizeof(UINT64));
        ShvOsFreeContiguousAlignedMemory((VOID*)ShvVpVirtualizedBitmap, size);
        ShvVpVirtualizedBitmap = NULL;
        ShvVpSecondLevelRoots = NULL;
        return SHV_STATUS_NO_RESOURCES;
    }
    __stosb((UINT8*)ShvVpFlushTargets, 0, CpuCount * sizeof(SHV_VP_FLUSH_TARGET));
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
        ShvOsFreeContiguousAlignedMemory((VOID*)ShvVpFlushTargets,
                                         ShvVpVirtualizedBitmapCount * sizeof(SHV_VP_FLUSH_TARGET));
        ShvVpVirtualizedBitmap = NULL;
        ShvVpSecondLevelRoots = NULL;
        ShvVpFlushTargets = NULL;
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
ShvVpPublishFlushTarget (
    _In_ PSHV_VP_DATA VpData
    )
{
    PSHV_VP_FLUSH_TARGET target = &ShvVpFlushTargets[VpData->ProcessorIndex];

    //
    // Called by the guest once it runs, as a flush IPI arriving before would
    // be taken by the OS's NMI handler. The NMI gate needs VpData before the
    // first flush IPI can arrive. Without x2APIC none can be sent at all
    //
    target->VpData = VpData;
    if (VpData->X2ApicEnabled != FALSE)
    {
        _InterlockedExchange64(&target->Icr,
                               ((INT64)VpData->X2ApicId << APIC_ICR_DESTINATION_SHIFT) |
                               APIC_ICR_LEVEL_ASSERT |
                               APIC_ICR_DELIVERY_NMI);
    }
}

VOID
ShvVpWithdrawFlushTarget (
    _In_ PSHV_VP_DATA VpData
    )
{
    PSHV_VP_FLUSH_TARGET target = &ShvVpFlushTargets[VpData->ProcessorIndex];
    UINT32 spins;

    //
    // Called in root mode as this processor leaves the guest, before it runs
    // the OS's code with the OS's IDT, whose NMI handler would take a flush
    // IPI for a hardware failure. Stop new senders, with a full barrier
    // before reading Senders, and wait for those that already picked this
    // processor to send their IPI
    //
    _InterlockedExchange64(&target->Icr, 0);
    while (target->Senders != 0)
    {
        _mm_pause();
    }

    //
    // Then take the flush IPI on its way, if any
    //
    for (spins = 0;
         (target->NmiInFlight != 0) && (spins < SHV_FLUSH_IPI_DRAIN_SPINS);
         spins++)
    {
        ShvArch->TakeHeldNmi(VpData);
        _mm_pause();
    }
    target->VpData = NULL;
}

UINT8
ShvVpTakeFlushIpi (
    _In_ PSHV_VP_DATA VpData
    )
{
    PSHV_VP_FLUSH_TARGET target = &ShvVpFlushTargets[VpData->ProcessorIndex];
    INT64 requested;

    //
    // An NMI is a flush IPI while one is on its way, and otherwise
    // unexpected. Clear NmiInFlight before reading Requested, with a full
    // barrier, so that a sender that found it set has its request covered
    // here, and one that finds it clear sends another flush IPI. Only this
    // processor takes its flush IPIs, one at a time, as NMIs are blocked
    // until the gate or the VM-Exit returns
    //
    if (_InterlockedExchange(&target->NmiInFlight, 0) == 0)
    {
        return FALSE;
    }

    requested = target->Requested;
    if (requested != target->Completed)
    {
        ShvArch->FlushGuestMappings(VpData);
        target->Completed = requested;
    }
    VpData->IpiCounts[ShvIpiFlushIpisReceived]++;
    return TRUE;
}

UINT8
ShvVpHandleRootNmi (
    _In_ UINT64 StackPointer
    )
{
    PSHV_VP_DATA vpData;
    UINT32 i;

    //
    // Called by the NMI gate of the idle IDT. Root mode runs on the stack in
    // the processor's VP data, which tells the processor the NMI arrived on.
    // An NMI elsewhere is unexpected
    //
    if (ShvVpFlushTargets == NULL)
    {
        return FALSE;
    }

    for (i = 0; i < ShvVpVirtualizedBitmapCount; i++)
    {
        vpData = ShvVpFlushTargets[i].VpData;
        if ((vpData != NULL) &&
            ((StackPointer - (UINT64)vpData->ShvStackLimit) < KERNEL_STACK_SIZE))
        {
            return ShvVpTakeFlushIpi(vpData);
        }
    }
    return FALSE;
}

VOID
ShvVpFlushGuestMappings (
    _In_ PSHV_VP_DATA VpData,
    _In_ PSHV_HYPERCALL Hypercall
    )
{
    UINT32 i;
    UINT64 root;
    UINT64 icr;
    INT64 requested;
    PSHV_VP_FLUSH_TARGET target;

    //
    // Nested under Hyper-V, the guest flushes the TLB of other processors with
    // hypercalls, which SimpleVisor forwards from root mode. Hyper-V then
    // flushes the mappings of SimpleVisor, not those the guest cached under
    // VPID or ASID 1. Flush those as well, on every processor, by flushing all
    // mappings cached for each processor's second level identity map. That is
    // more than the guest asked for, but its TLB refills from the same map.
    //
    if (VpData->HyperVGuestMappingFlush != FALSE)
    {
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
        return;
    }

    //
    // Without Hyper-V's flush hypercall, flush the guest's mappings with
    // SimpleVisor's own flush IPIs. Each virtualized processor takes the IPI
    // in root mode and flushes the mappings cached for its second level
    // identity map, and the sender flushes its own. Send them all first
    //
    for (i = 0; i < ShvVpVirtualizedBitmapCount; i++)
    {
        root = ShvVpSecondLevelRoots[i];
        if (root == 0)
        {
            continue;
        }

        if (i == VpData->ProcessorIndex)
        {
            ShvArch->FlushGuestMappings(VpData);
            VpData->IpiCounts[ShvIpiFlushIpisSent]++;
            continue;
        }

        //
        // Request the flush, and send the flush IPI unless one is already on
        // its way, which then covers this request. Senders counts this
        // sender while Icr is used. Icr is 0 while the processor cannot take
        // the IPI
        //
        target = &ShvVpFlushTargets[i];
        _InterlockedIncrement(&target->Senders);
        icr = (UINT64)target->Icr;
        if (icr != 0)
        {
            _InterlockedIncrement64(&target->Requested);
            if (_InterlockedExchange(&target->NmiInFlight, 1) == 0)
            {
                if (ShvUtilAccessMsr(VpData, APIC_ICR, TRUE, &icr) != FALSE)
                {
                    VpData->IpiCounts[ShvIpiFlushIpisSent]++;
                }
                else
                {
                    //
                    // The local APIC left x2APIC mode, so no flush IPI can be
                    // sent to that processor anymore, and nobody waits for one
                    //
                    _InterlockedExchange64(&target->Icr, 0);
                    _InterlockedExchange(&target->NmiInFlight, 0);
                }
            }
        }
        _InterlockedDecrement(&target->Senders);
    }

    //
    // The guest takes the flush as done on every processor once its
    // hypercall returns, so wait for each processor to flush, or to leave
    // the guest. Take the flush IPIs sent to this processor meanwhile, which
    // SVM would hold until the next VMRUN, as their senders may be waiting
    // for this processor in turn
    //
    for (i = 0; i < ShvVpVirtualizedBitmapCount; i++)
    {
        if (i == VpData->ProcessorIndex)
        {
            continue;
        }

        target = &ShvVpFlushTargets[i];
        requested = target->Requested;
        while ((target->Icr != 0) && (target->Completed < requested))
        {
            ShvArch->TakeHeldNmi(VpData);
            _mm_pause();
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

UINT8
ShvVpDetectUnderlyingHypervisor (
    _In_ PSHV_VP_DATA VpData
    )
{
    INT32 cpuInfo[4];
    UINT32 maxLeaf;

    //
    // SimpleVisor is not running yet, so CPUID reaches the processor or the
    // hypervisor we are nested under, which sets the Hypervisor Present-bit
    //
    __cpuid(cpuInfo, 1);
    VpData->UnderlyingHypervisor = (cpuInfo[2] & HYPERV_HYPERVISOR_PRESENT_BIT) != 0;
    if (VpData->UnderlyingHypervisor == FALSE)
    {
        return TRUE;
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
    if (VpData->UnderlyingHypervisorIsHyperV == FALSE)
    {
        return TRUE;
    }
    maxLeaf = (UINT32)cpuInfo[0];

    //
    // Check whether Hyper-V lets SimpleVisor idle in the guest idle MSR
    //
    __cpuid(cpuInfo, HYPERV_CPUID_FEATURES);
    VpData->HyperVGuestIdle = (cpuInfo[0] & HYPERV_FEATURE_GUEST_IDLE_MSR) != 0;

    //
    // Check whether Hyper-V can flush the guest's cached mappings on every
    // processor, and keep the physical address of that hypercall's input
    //
    if (maxLeaf >= HYPERV_CPUID_NESTED_FEATURES)
    {
        __cpuid(cpuInfo, HYPERV_CPUID_NESTED_FEATURES);
        VpData->HyperVGuestMappingFlush =
            (cpuInfo[0] & HYPERV_NESTED_GUEST_MAPPING_FLUSH) != 0;
        VpData->GuestMappingFlushInputPhysicalAddress =
            ShvOsGetPhysicalAddress(VpData->GuestMappingFlushInput);
    }

    //
    // The guest keeps the hypercalls Hyper-V recommended at boot for
    // switching address spaces and flushing the TLB. The VM-Exit handlers
    // forward the flush hypercalls and compensate for them with Hyper-V's
    // flush hypercall or SimpleVisor's own flush IPIs, and handle the switch
    // hypercall by switching the guest's CR3, so neither keeps SimpleVisor
    // from loading
    //
    return TRUE;
}

INT32
ShvVpLaunch (
    _In_ PSHV_VP_DATA VpData
    )
{
    INT32 status;
    UINT64 value;
    //
    // Being nested changes how CPUID and the identity map are handled, and
    // can keep SimpleVisor from running at all
    //
    if (ShvVpDetectUnderlyingHypervisor(VpData) == FALSE)
    {
        return SHV_STATUS_NOT_AVAILABLE;
    }

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
    // Remember whether the guest's local APIC is in x2APIC mode, which the
    // flush IPIs need, and this processor's x2APIC ID, which they target.
    // Windows enables x2APIC at boot when the processor offers it, and the
    // lab sets the x2apicpolicy BCD option to make it do so. The ID is
    // published only once the guest runs, so a flush IPI never reaches this
    // processor while it still runs the OS's code
    //
    value = 0;
    if (ShvUtilAccessMsr(VpData, APIC_BASE, FALSE, &value) != FALSE)
    {
        VpData->X2ApicEnabled = (value & APIC_BASE_X2APIC) != 0;
        if ((VpData->X2ApicEnabled != FALSE) &&
            (ShvUtilAccessMsr(VpData, APIC_ID, FALSE, &value) != FALSE))
        {
            VpData->X2ApicId = (UINT32)value;
        }
    }

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
    else
    {
        //
        // The guest runs, back on the OS's stack, so this processor can take
        // flush IPIs from now on
        //
        ShvVpPublishFlushTarget(Data);
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
    __cpuidex(cpuInfo, SHV_CPUID_LEAF, SHV_CPUID_UNLOAD);

    //
    // If SimpleVisor is disabled for some reason, CPUID will return the values
    // of the highest valid CPUID. We use a magic value to make sure we really
    // are loaded and returned something valid.
    //
    if (cpuInfo[2] == SHV_CPUID_UNLOAD_RESULT)
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
        for (i = 0; i < ShvIpiCount; i++)
        {
            _InterlockedExchangeAdd64(&Context->IpiCounts[i], (INT64)vpData->IpiCounts[i]);
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
