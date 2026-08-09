/*++

Copyright (c) Alex Ionescu.  All rights reserved.

Module Name:

    shvos.c

Abstract:

    This module implements the OS-facing Windows stubs for SimpleVisor.

Author:

    Alex Ionescu (@aionescu) 29-Aug-2016 - Initial version

Environment:

    Kernel mode only.

--*/

#include <ntifs.h>
#include <stdarg.h>
#include "shv_x.h"
#pragma warning(disable:4221)
#pragma warning(disable:4204)

NTKERNELAPI
_IRQL_requires_max_(APC_LEVEL)
_IRQL_requires_min_(PASSIVE_LEVEL)
_IRQL_requires_same_
VOID
KeGenericCallDpc (
    _In_ PKDEFERRED_ROUTINE Routine,
    _In_opt_ PVOID Context
    );

NTKERNELAPI
_IRQL_requires_(DISPATCH_LEVEL)
_IRQL_requires_same_
VOID
KeSignalCallDpcDone (
    _In_ PVOID SystemArgument1
    );

NTKERNELAPI
_IRQL_requires_(DISPATCH_LEVEL)
_IRQL_requires_same_
LOGICAL
KeSignalCallDpcSynchronize (
    _In_ PVOID SystemArgument2
    );

DRIVER_INITIALIZE DriverEntry;

DECLSPEC_NORETURN
VOID
__cdecl
ShvOsRestoreContext2 (
    _In_ PCONTEXT ContextRecord,
    _In_opt_ struct _EXCEPTION_RECORD * ExceptionRecord
    );

VOID
ShvVmxCleanup (
    _In_ UINT16 Data,
    _In_ UINT16 Teb
    );

typedef struct _SHV_DPC_CONTEXT
{
    PSHV_CPU_CALLBACK Routine;
    struct _SHV_CALLBACK_CONTEXT* Context;
} SHV_DPC_CONTEXT, *PSHV_DPC_CONTEXT;

#define KGDT64_R3_DATA      0x28
#define KGDT64_R3_CMTEB     0x50

PVOID g_PowerCallbackRegistration;

//
// The power callback can run while DriverEntry loads the hypervisor, so both
// take this lock to load or unload it. The hypervisor is unloaded while the
// system sleeps
//
KMUTEX g_ShvLock;
BOOLEAN g_ShvLoaded;
BOOLEAN g_ShvSleeping;

NTSTATUS
FORCEINLINE
ShvOsErrorToError (
    INT32 Error
    )
{
    //
    // Convert the possible SimpleVisor errors into NT Hyper-V Errors
    //
    if (Error == SHV_STATUS_NOT_AVAILABLE)
    {
        return STATUS_HV_FEATURE_UNAVAILABLE;
    }
    if (Error == SHV_STATUS_NO_RESOURCES)
    {
        return STATUS_HV_NO_RESOURCES;
    }
    if (Error == SHV_STATUS_NOT_PRESENT)
    {
        return STATUS_HV_NOT_PRESENT;
    }
    if (Error == SHV_STATUS_SUCCESS)
    {
        return STATUS_SUCCESS;
    }

    //
    // Unknown/unexpected error
    //
    return STATUS_UNSUCCESSFUL;
}

VOID
ShvOsDpcRoutine (
    _In_ struct _KDPC *Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2
    )
{
    PSHV_DPC_CONTEXT dpcContext = DeferredContext;
    UNREFERENCED_PARAMETER(Dpc);

    __analysis_assume(DeferredContext != NULL);
    __analysis_assume(SystemArgument1 != NULL);
    __analysis_assume(SystemArgument2 != NULL);

    //
    // Execute the internal callback function
    //
    dpcContext->Routine(dpcContext->Context);

    //
    // During unload SimpleVisor restores the guest context from the VM-Exit
    // handler, after VMX loaded the host selectors with their RPL bits
    // cleared. The x64 kernel does not expect kernel-mode code to change any
    // segments, so DS and ES are stuck at 0x20, and FS at 0x50, until the
    // next context switch.
    //
    // If the DPC happened to have interrupted either the idle thread or system
    // thread, that's perfectly fine (albeit unusual). If the DPC interrupted a
    // 64-bit long-mode thread, that's also fine. However if the DPC interrupts
    // a thread in compatibility-mode, running as part of WoW64, it will hit a
    // GPF instantaneously and crash.
    //
    // Thus, set the segments to their correct value, one more time, as a fix.
    //
    ShvVmxCleanup(KGDT64_R3_DATA | RPL_MASK, KGDT64_R3_CMTEB | RPL_MASK);

    //
    // Wait for all DPCs to synchronize at this point
    //
    KeSignalCallDpcSynchronize(SystemArgument2);

    //
    // Mark the DPC as being complete
    //
    KeSignalCallDpcDone(SystemArgument1);
}

INT32
ShvOsPrepareProcessor (
    _In_ PSHV_VP_DATA VpData
    )
{
    //
    // Nothing to do on NT, only return SHV_STATUS_SUCCESS
    //
    UNREFERENCED_PARAMETER(VpData);
    return SHV_STATUS_SUCCESS;
}

VOID
ShvOsUnprepareProcessor (
    _In_ PSHV_VP_DATA VpData
    )
{
    //
    // When running in VMX root mode, the processor will set limits of the
    // GDT and IDT to 0xFFFF (notice that there are no Host VMCS fields to
    // set these values). This causes problems with PatchGuard, which will
    // believe that the GDTR and IDTR have been modified by malware, and
    // eventually crash the system. Since we know what the original state
    // of the GDTR and IDTR was, simply restore it now.
    //
    __lgdt(&VpData->SpecialRegisters.Gdtr.Limit);
    __lidt(&VpData->SpecialRegisters.Idtr.Limit);
}

VOID
PowerCallback (
    _In_opt_ PVOID CallbackContext,
    _In_opt_ PVOID Argument1,
    _In_opt_ PVOID Argument2
    )
{
    UNREFERENCED_PARAMETER(CallbackContext);

    //
    // Ignore non-Sx changes
    //
    if (Argument1 != (PVOID)PO_CB_SYSTEM_STATE_LOCK)
    {
        return;
    }

    //
    // Check if this is S0->Sx, or Sx->S0
    //
    KeWaitForSingleObject(&g_ShvLock, Executive, KernelMode, FALSE, NULL);
    if (ARGUMENT_PRESENT(Argument2))
    {
        //
        // Reload the hypervisor. If that fails, the system runs without it
        // until the next resume
        //
        g_ShvSleeping = FALSE;
        if (g_ShvLoaded == FALSE)
        {
            g_ShvLoaded = (ShvLoad() == SHV_STATUS_SUCCESS);
        }
    }
    else
    {
        //
        // Unload the hypervisor, if it runs
        //
        g_ShvSleeping = TRUE;
        if (g_ShvLoaded != FALSE)
        {
            ShvUnload();
            g_ShvLoaded = FALSE;
        }
    }
    KeReleaseMutex(&g_ShvLock, FALSE);
}

VOID
ShvOsFreeContiguousAlignedMemory (
    _In_ PVOID BaseAddress
    )
{
    //
    // Free the memory
    //
    MmFreeContiguousMemory(BaseAddress);
}

PVOID
ShvOsAllocateContigousAlignedMemory (
    _In_ SIZE_T Size
    )
{
    PHYSICAL_ADDRESS lowest, highest;

    //
    // The entire address range is OK for this allocation
    //
    lowest.QuadPart = 0;
    highest.QuadPart = lowest.QuadPart - 1;

    //
    // Allocate a contiguous chunk of RAM to back this allocation and make sure
    // that it is RW only, instead of RWX, by using the new Windows 8 API.
    //
    return MmAllocateContiguousNodeMemory(Size,
                                          lowest,
                                          highest,
                                          lowest,
                                          PAGE_READWRITE,
                                          KeGetCurrentNodeNumber());
}

ULONGLONG
ShvOsGetPhysicalAddress (
    _In_ PVOID BaseAddress
    )
{
    //
    // Let the memory manager convert it
    //
    return MmGetPhysicalAddress(BaseAddress).QuadPart;
}

//
// The physical memory ranges mapped into system address space at load, for
// reading guest memory from root mode, where no OS service may run. Each
// range maps to the virtual address at the same offset, so that a physical
// address reads from the same virtual one
//
#define SHV_PHYSICAL_RANGE_COUNT    32

typedef struct _SHV_PHYSICAL_RANGE
{
    UINT64 PhysicalBase;
    UINT64 VirtualBase;
    UINT64 Length;
} SHV_PHYSICAL_RANGE;

static SHV_PHYSICAL_RANGE ShvOsPhysicalRanges[SHV_PHYSICAL_RANGE_COUNT];
static UINT32 ShvOsPhysicalRangeCount;

INT32
ShvOsInitializePhysicalMemory (
    VOID
    )
{
    PPHYSICAL_MEMORY_RANGE ranges;
    UINT32 i;

    //
    // Map every physical memory range the platform reports. The identity
    // map of the hypervisor makes guest physical addresses equal physical
    // ones, so reading at the same offset gets the guest's memory
    //
    ranges = MmGetPhysicalMemoryRanges();
    if (ranges == NULL)
    {
        return SHV_STATUS_NO_RESOURCES;
    }

    ShvOsPhysicalRangeCount = 0;
    for (i = 0; ranges[i].NumberOfBytes.QuadPart != 0; i++)
    {
        if (ShvOsPhysicalRangeCount >= SHV_PHYSICAL_RANGE_COUNT)
        {
            ShvOsUninitializePhysicalMemory();
            ExFreePool(ranges);
            return SHV_STATUS_NO_RESOURCES;
        }

        ShvOsPhysicalRanges[ShvOsPhysicalRangeCount].PhysicalBase =
            ranges[i].BaseAddress.QuadPart;
        ShvOsPhysicalRanges[ShvOsPhysicalRangeCount].VirtualBase =
            (UINT64)MmMapIoSpace(ranges[i].BaseAddress,
                                 ranges[i].NumberOfBytes,
                                 MmNonCached);
        ShvOsPhysicalRanges[ShvOsPhysicalRangeCount].Length =
            ranges[i].NumberOfBytes.QuadPart;
        if (ShvOsPhysicalRanges[ShvOsPhysicalRangeCount].VirtualBase == 0)
        {
            ShvOsUninitializePhysicalMemory();
            ExFreePool(ranges);
            return SHV_STATUS_NO_RESOURCES;
        }
        ShvOsPhysicalRangeCount++;
    }

    ExFreePool(ranges);
    return SHV_STATUS_SUCCESS;
}

VOID
ShvOsUninitializePhysicalMemory (
    VOID
    )
{
    UINT32 i;

    //
    // Unmap the ranges mapped at load, in reverse order
    //
    for (i = ShvOsPhysicalRangeCount; i > 0; i--)
    {
        MmUnmapIoSpace((PVOID)ShvOsPhysicalRanges[i - 1].VirtualBase,
                       (SIZE_T)ShvOsPhysicalRanges[i - 1].Length);
    }
    ShvOsPhysicalRangeCount = 0;
}

UINT8
ShvOsReadPhysical (
    _In_ UINT64 PhysicalAddress,
    _In_ UINT32 Length,
    _Out_ VOID* Buffer
    )
{
    UINT32 i;

    //
    // Find the range the physical address falls into, which must cover the
    // whole read, and copy it out
    //
    for (i = 0; i < ShvOsPhysicalRangeCount; i++)
    {
        if ((PhysicalAddress >= ShvOsPhysicalRanges[i].PhysicalBase) &&
            (PhysicalAddress + Length <=
             ShvOsPhysicalRanges[i].PhysicalBase + ShvOsPhysicalRanges[i].Length))
        {
            __movsb(Buffer,
                    (VOID*)(ShvOsPhysicalRanges[i].VirtualBase +
                            (PhysicalAddress - ShvOsPhysicalRanges[i].PhysicalBase)),
                    Length);
            return TRUE;
        }
    }
    return FALSE;
}

VOID
ShvOsRunCallbackOnProcessors (
    _In_ PSHV_CPU_CALLBACK Routine,
    _In_opt_ PVOID Context
    )
{
    SHV_DPC_CONTEXT dpcContext;

    //
    // Wrap the internal routine and context under a Windows DPC
    //
    dpcContext.Routine = Routine;
    dpcContext.Context = Context;
    KeGenericCallDpc(ShvOsDpcRoutine, &dpcContext);
}

VOID
ShvOsRestoreContext(
    _In_ PCONTEXT ContextRecord
    )
{
    ShvOsRestoreContext2(ContextRecord, NULL);
}

VOID
ShvOsCaptureContext (
    _In_ PCONTEXT ContextRecord
    )
{
    //
    // Windows provides a nice OS function to do this
    //
    RtlCaptureContext(ContextRecord);
}

INT32
ShvOsGetCurrentProcessorNumber (
    VOID
    )
{
    //
    // Get the group-wide CPU index
    //
    return (INT32)KeGetCurrentProcessorNumberEx(NULL);
}

INT32
ShvOsGetActiveProcessorCount (
    VOID
    )
{
    //
    // Get the group-wide CPU count
    //
    return (INT32)KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
}

INT32
ShvOsGetMaximumProcessorCount (
    VOID
    )
{
    //
    // Get the group-wide CPU count, including processors that are not active
    // or not added yet, which KeGetCurrentProcessorNumberEx stays below
    //
    return (INT32)KeQueryMaximumProcessorCountEx(ALL_PROCESSOR_GROUPS);
}

VOID
ShvOsDebugPrint (
    _In_ PCCH Format,
    ...
    )
{
    va_list arglist;

    //
    // Call the debugger API
    //
    va_start(arglist, Format);
    vDbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, Format, arglist);
    va_end(arglist);
}

VOID
DriverUnload (
    _In_ PDRIVER_OBJECT DriverObject
    )
{
    UNREFERENCED_PARAMETER(DriverObject);

    //
    // Unregister the power callback. We would not have loaded without it.
    // This waits for a callback that is running, so none runs from here on
    //
    ExUnregisterCallback(g_PowerCallbackRegistration);

    //
    // Unload the hypervisor, if it runs
    //
    if (g_ShvLoaded != FALSE)
    {
        ShvUnload();
        g_ShvLoaded = FALSE;
    }
}

NTSTATUS
DriverEntry (
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS status;
    PCALLBACK_OBJECT callbackObject;
    UNICODE_STRING callbackName =
        RTL_CONSTANT_STRING(L"\\Callback\\PowerState");
    OBJECT_ATTRIBUTES objectAttributes =
        RTL_CONSTANT_OBJECT_ATTRIBUTES(&callbackName,
                                       OBJ_CASE_INSENSITIVE |
                                       OBJ_KERNEL_HANDLE);
    UNREFERENCED_PARAMETER(RegistryPath);

    //
    // Make the driver (and SHV itself) unloadable
    //
    DriverObject->DriverUnload = DriverUnload;
    KeInitializeMutex(&g_ShvLock, 0);

    //
    // Create the power state callback
    //
    status = ExCreateCallback(&callbackObject, &objectAttributes, FALSE, TRUE);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    //
    // Now register our routine with this callback
    //
    g_PowerCallbackRegistration = ExRegisterCallback(callbackObject,
                                                     PowerCallback,
                                                     NULL);

    //
    // Dereference it in both cases -- either it's registered, so that is now
    // taking a reference, and we'll unregister later, or it failed to register
    // so we failing now, and it's gone.
    //
    ObDereferenceObject(callbackObject);

    //
    // Fail if we couldn't register the power callback
    //
    if (g_PowerCallbackRegistration == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //
    // Load the hypervisor, unless the system is going to sleep, in which case
    // the power callback loads it on resume
    //
    status = STATUS_SUCCESS;
    KeWaitForSingleObject(&g_ShvLock, Executive, KernelMode, FALSE, NULL);
    if (g_ShvSleeping == FALSE)
    {
        status = ShvOsErrorToError(ShvLoad());
        g_ShvLoaded = NT_SUCCESS(status);
    }
    KeReleaseMutex(&g_ShvLock, FALSE);

    //
    // If load of the hypervisor happened to fail, unregister previously registered
    // power callback, otherwise we would get BSOD on shutdown. Not under the lock,
    // as this waits for a callback that may be waiting for it
    //
    if (!NT_SUCCESS(status))
    {
        ExUnregisterCallback(g_PowerCallbackRegistration);
    }

    return status;
}

