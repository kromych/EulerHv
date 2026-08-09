/*++

Copyright (c) Alex Ionescu.  All rights reserved.

Header Name:

    shv.h

Abstract:

    This header defines the structures and functions of the Simple Hyper Visor.

Author:

    Alex Ionescu (@aionescu) 14-Mar-2016 - Initial version

Environment:

    Kernel mode only.

--*/

#pragma once
#pragma warning(disable:4201)
#pragma warning(disable:4214)

#ifndef __BASE_H__
#include <basetsd.h>
#endif
#define _INC_MALLOC
#include <intrin.h>
#include "ntint.h"
#include "shv_x.h"

typedef struct _SHV_VP_STATE
{
    PCONTEXT VpRegs;
    PSHV_VP_DATA VpData;
    uintptr_t GuestRip;
    uintptr_t GuestRsp;
    uintptr_t GuestEFlags;
    UINT16 ExitReason;
    UINT8 ExitVm;
    UINT8 SkipRipAdvance;
} SHV_VP_STATE, *PSHV_VP_STATE;

typedef struct _SHV_CALLBACK_CONTEXT
{
    UINT64 Cr3;
    volatile long InitCount;
    INT32 FailedCpu;
    INT32 FailureStatus;
    volatile INT64 ExitCounts[SHV_EXIT_REASON_COUNT];
    volatile INT64 TlbHypercallCounts[ShvTlbHypercallCount];
    volatile INT64 IpiCounts[ShvIpiCount];
} SHV_CALLBACK_CONTEXT, *PSHV_CALLBACK_CONTEXT;

//
// The hardware virtualization backend. ShvVpInitialize calls the routines in
// the order below, on the logical processor being virtualized
//
typedef struct _SHV_ARCH
{
    //
    // Whether this processor and its firmware support the backend. Captures
    // the capabilities the other routines rely on
    //
    UINT8 (*Probe) (_In_ PSHV_VP_DATA VpData);

    //
    // Build the second level identity map of physical memory
    //
    VOID (*BuildPaging) (_In_ PSHV_VP_DATA VpData);

    //
    // Enter root mode with the guest state structure ready to fill in
    //
    UINT8 (*Enable) (_In_ PSHV_VP_DATA VpData);

    //
    // Fill in the guest and host state from the captured processor state
    //
    VOID (*Setup) (_In_ PSHV_VP_DATA VpData);

    //
    // Launch the guest and run the VM-Exit loop. Returns only on failure,
    // after leaving root mode. On success the guest resumes in
    // ShvVpRestoreAfterLaunch
    //
    INT32 (*Run) (_In_ PSHV_VP_DATA VpData);

    //
    // Handle one VM-Exit, called from the VM-Exit loop
    //
    VOID (*HandleExit) (_In_ PSHV_VP_STATE VpState);

    //
    // Flush the guest's mappings cached for this processor's second level
    // identity map, called when SimpleVisor's flush IPI arrives
    //
    VOID (*FlushGuestMappings) (_In_ PSHV_VP_DATA VpData);
} SHV_ARCH, *PSHV_ARCH;

extern const SHV_ARCH ShvVmxArch;
extern const SHV_ARCH ShvSvmArch;
extern const SHV_ARCH* ShvArch;

UINT8
ShvSvmIsAmd (
    VOID
    );

VOID
ShvVmxInjectNmi (
    _In_ PSHV_VP_STATE VpState
    );

VOID
ShvSvmInjectNmi (
    _In_ PSHV_VP_STATE VpState
    );

SHV_CPU_CALLBACK ShvVpLoadCallback;
SHV_CPU_CALLBACK ShvVpUnloadCallback;

INT32
ShvVpAllocateTracking (
    _In_ UINT32 CpuCount
    );

VOID
ShvVpFreeTracking (
    VOID
    );

VOID
ShvVpMarkVirtualized (
    _In_ UINT32 ProcessorIndex
    );

VOID
ShvVpUnmarkVirtualized (
    _In_ UINT32 ProcessorIndex
    );

//
// Issues a hypercall of SimpleVisor's own to Hyper-V, with VMCALL or VMMCALL,
// and returns its result value
//
typedef
UINT64
SHV_HYPERCALL (
    _In_ UINT64 Input,
    _In_ UINT64 InputPhysicalAddress,
    _In_ UINT64 OutputPhysicalAddress
    );
typedef SHV_HYPERCALL *PSHV_HYPERCALL;

VOID
ShvVpSetSecondLevelRoot (
    _In_ UINT32 ProcessorIndex,
    _In_ UINT64 Root
    );

//
// Publish or clear the flush IPI value of a processor: its x2APIC ID as the
// ICR destination, with the flush vector. 0 is not a value, so it marks the
// processor as not running the guest
//
VOID
ShvVpSetApicId (
    _In_ UINT32 ProcessorIndex,
    _In_ UINT64 Icr
    );

VOID
ShvVpFlushGuestMappings (
    _In_ PSHV_VP_DATA VpData,
    _In_ PSHV_HYPERCALL Hypercall
    );

VOID
ShvUtilConvertGdtEntry (
    _In_ VOID* GdtBase,
    _In_ UINT16 Offset,
    _Out_ PVMX_GDTENTRY64 VmxGdtEntry
    );

UINT32
ShvUtilAdjustMsr (
    _In_ LARGE_INTEGER ControlValue,
    _In_ UINT32 DesiredValue
    );

VOID
ShvUtilFlushTlb (
    VOID
    );

//
// The interrupt stubs of the idle IDT, one every SHV_IDLE_STUB_SIZE bytes from
// IDT_FIRST_INTERRUPT on
//
#define SHV_IDLE_STUB_SIZE  16

VOID
ShvIdleStubs (
    VOID
    );

//
// The idle IDT's NMI gate, which stops the processor for the debugger, as
// no NMI should arrive
//
VOID
ShvIdleNmiStub (
    VOID
    );

//
// Take one pending interrupt through the idle IDT, returning its vector, or
// 0 if there was none. The idle IDT must not be loaded by the caller
//
UINT32
ShvUtilTakeInterrupt (
    _In_ PSHV_VP_DATA VpData
    );

UINT32
ShvIdleWaitForInterrupt (
    VOID
    );

VOID
ShvUtilBuildIdleIdt (
    _In_ PSHV_VP_DATA VpData
    );

UINT32
ShvUtilIdle (
    _In_ PSHV_VP_DATA VpData
    );

//
// Read and write an MSR, returning FALSE if it raised #GP, which they need
// ShvMsrFault as the #GP gate for. ShvUtilAccessMsr loads the idle IDT, which
// has it
//
UINT8
ShvMsrRead (
    _In_ UINT32 Msr,
    _Out_ UINT64* Value
    );

UINT8
ShvMsrWrite (
    _In_ UINT32 Msr,
    _In_ UINT64 Value
    );

VOID
ShvMsrFault (
    VOID
    );

UINT8
ShvUtilAccessMsr (
    _In_ PSHV_VP_DATA VpData,
    _In_ UINT32 Msr,
    _In_ UINT8 Write,
    _Inout_ UINT64* Value
    );

UINT8
ShvUtilCountHypercall (
    _In_ PSHV_VP_DATA VpData,
    _In_ UINT64 HypercallInput
    );

VOID
ShvUtilMtrrInitialize (
    _In_ PSHV_VP_DATA VpData
    );

UINT32
ShvUtilMtrrGetMemoryType (
    _In_ PSHV_VP_DATA VpData,
    _In_ UINT64 PageAddress,
    _In_ UINT64 PageSize
    );

PSHV_VP_DATA
ShvVpAllocateData (
    _In_ UINT32 CpuCount
    );

VOID
ShvVpFreeData  (
    _In_ PSHV_VP_DATA Data,
    _In_ UINT32 CpuCount
    );

DECLSPEC_NORETURN
VOID
ShvVpRestoreAfterLaunch (
    VOID
    );

//
// OS Layer
//
DECLSPEC_NORETURN
VOID
__cdecl
ShvOsRestoreContext (
    _In_ PCONTEXT ContextRecord
    );

VOID
ShvOsCaptureContext (
    _In_ PCONTEXT ContextRecord
    );

VOID
ShvOsUnprepareProcessor (
    _In_ PSHV_VP_DATA VpData
    );

INT32
ShvOsPrepareProcessor (
    _In_ PSHV_VP_DATA VpData
    );

INT32
ShvOsGetActiveProcessorCount (
    VOID
    );

INT32
ShvOsGetMaximumProcessorCount (
    VOID
    );

INT32
ShvOsGetCurrentProcessorNumber (
    VOID
    );

VOID
ShvOsFreeContiguousAlignedMemory (
    _In_ VOID* BaseAddress,
    _In_ size_t Size
    );

VOID*
ShvOsAllocateContigousAlignedMemory (
    _In_ size_t Size
    );

UINT64
ShvOsGetPhysicalAddress (
    _In_ VOID* BaseAddress
    );

#ifndef __BASE_H__
VOID
ShvOsDebugPrint (
    _In_ const char* Format,
    ...
    );
#else
VOID
ShvOsDebugPrintWide (
    _In_ const CHAR16* Format,
    ...
    );
#define ShvOsDebugPrint(format, ...) ShvOsDebugPrintWide(_CRT_WIDE(format), __VA_ARGS__)
#endif

VOID
ShvOsRunCallbackOnProcessors (
    _In_ PSHV_CPU_CALLBACK Routine,
    _In_opt_ VOID* Context
    );

extern PSHV_VP_DATA* ShvGlobalData;

