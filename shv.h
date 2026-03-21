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
} SHV_ARCH, *PSHV_ARCH;

extern const SHV_ARCH ShvVmxArch;
extern const SHV_ARCH ShvSvmArch;
extern const SHV_ARCH* ShvArch;

UINT8
ShvSvmIsAmd (
    VOID
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
ShvUtilMtrrInitialize (
    _In_ PSHV_VP_DATA VpData
    );

UINT32
ShvUtilMtrrAdjustEffectiveMemoryType (
    _In_ PSHV_VP_DATA VpData,
    _In_ UINT64 LargePageAddress,
    _In_ UINT32 CandidateMemoryType
    );

UINT32
ShvUtilMtrrGetHugePageMemoryType (
    _In_ PSHV_VP_DATA VpData,
    _In_ UINT64 HugePageAddress,
    _In_ UINT32 DefaultMemoryType
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

