/*++

Copyright (c) Alex Ionescu.  All rights reserved.

Header Name:

    shv.h

Abstract:

    This header defines the structures and functions of the Simple Hyper Visor.

Author:

    Alex Ionescu (@aionescu) 14-Mar-2016 - Initial version
    kromych (vdso@mailbox.org) 07-Feb-2026 - Backend interface, flush IPIs, idle

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

    //
    // Take an NMI that root mode holds pending, through the idle IDT's NMI
    // gate. SVM holds NMIs while the GIF is clear, and VT-x delivers them
    // in root mode right away, so there is nothing to take
    //
    VOID (*TakeHeldNmi) (_In_ PSHV_VP_DATA VpData);

    //
    // Pass an NMI that is not a flush IPI on to the guest, whose own NMI
    // handler takes it as without SimpleVisor, such as when Windows freezes
    // the processors for the debugger. Called for an NMI exit, or for an NMI
    // in root mode
    //
    VOID (*ReflectNmi) (_In_ PSHV_VP_DATA VpData);
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
// Make this processor a target of the flush IPIs once its guest runs, and
// stop that in root mode as it leaves the guest, taking the flush IPIs that
// are still on their way
//
VOID
ShvVpPublishFlushTarget (
    _In_ PSHV_VP_DATA VpData
    );

VOID
ShvVpWithdrawFlushTarget (
    _In_ PSHV_VP_DATA VpData
    );

//
// Take an NMI as a flush IPI and flush, if one is on its way to this
// processor, returning FALSE for any other NMI. ShvVpHandleRootNmi does so
// for the NMI gate of the idle IDT, finding the processor by its stack
// pointer
//
UINT8
ShvVpTakeFlushIpi (
    _In_ PSHV_VP_DATA VpData
    );

UINT8
ShvVpHandleRootNmi (
    _In_ UINT64 StackPointer
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
// The idle IDT's NMI gate, which takes SimpleVisor's flush IPIs, passes the
// guest's NMIs on to it where the backend can, and stops the processor for
// the debugger on any other NMI
//
VOID
ShvIdleNmiStub (
    VOID
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

//
// Get the CR3 that the guest's HvCallSwitchVirtualAddressSpace switches to,
// returning FALSE if its input is not in the registers
//
UINT8
ShvUtilGetSwitchAddressSpace (
    _In_ PSHV_VP_STATE VpState,
    _Out_ UINT64* AddressSpace
    );

UINT8
ShvUtilCountHypercall (
    _In_ PSHV_VP_DATA VpData,
    _In_ UINT64 HypercallInput
    );

//
// Record a VM-Exit in the exit trace, the debugger's ring of the last exits on
// all processors, and print its newest entries. MSR exits record the MSR as
// the interruption info, and the value written, if any, as the qualification.
// Hypercall exits record the low half of the hypercall input value (RCX), with
// the call code, and its input address (RDX) likewise.
// The trace also records events of SimpleVisor's own, as exit reasons no exit
// has:
//
//   SHV_TRACE_FLUSH_REQUEST    a flush requested of another processor: the
//                              interruption info is its index, with bit 31 set
//                              if this sent the flush IPI, and the
//                              qualification its Requested count
//   SHV_TRACE_NMI              an NMI checked for a flush IPI: the
//                              interruption info is 1 if one was on its way,
//                              and the qualification the Requested count read
//
#define SHV_TRACE_FLUSH_REQUEST     0xFF00
#define SHV_TRACE_NMI               0xFF01
#define SHV_TRACE_FLUSH_IPI_SENT    0x80000000

VOID
ShvUtilTraceExit (
    _In_ UINT32 ProcessorIndex,
    _In_ UINT16 ExitReason,
    _In_ UINT32 InterruptionInfo,
    _In_ UINT64 GuestRip,
    _In_ UINT64 Qualification
    );

VOID
ShvUtilTraceDump (
    _In_ UINT32 Count
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

