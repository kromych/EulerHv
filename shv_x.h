/*++

Copyright (c) Alex Ionescu.  All rights reserved.

Header Name:

    shv_x.h

Abstract:

    This header defines the externally visible structures and functions of the
    Simple Hyper Visor which are visible between the OS layer and SimpleVisor.

Author:

    Alex Ionescu (@aionescu) 29-Aug-2016 - Initial version

Environment:

    Kernel mode only.

--*/

#pragma once

#include "vmx.h"
#include "svm.h"

#define SHV_STATUS_SUCCESS          0
#define SHV_STATUS_NOT_AVAILABLE    -1
#define SHV_STATUS_NO_RESOURCES     -2
#define SHV_STATUS_NOT_PRESENT      -3

#define _1GB                        (1 * 1024 * 1024 * 1024)
#define _2MB                        (2 * 1024 * 1024)

#define SHV_STACK_CANARY            0x6B63617453766853 // 'kcatSvhS'

//
// VM-Exit reasons counted separately. Higher ones share the last counter
//
#define SHV_EXIT_REASON_COUNT       80

//
// EPT PML4 entries SimpleVisor can populate. The first maps the low 512GB with
// 2MB pages and the rest use 1GB pages when supported, for up to 8TB
//
#define SHV_EPT_PML4E_COUNT         16

struct _SHV_CALLBACK_CONTEXT;

typedef
void
SHV_CPU_CALLBACK (
    _In_ struct _SHV_CALLBACK_CONTEXT* Context
    );
typedef SHV_CPU_CALLBACK *PSHV_CPU_CALLBACK;

typedef struct _SHV_SPECIAL_REGISTERS
{
    UINT64 Cr0;
    UINT64 Cr3;
    UINT64 Cr4;
    UINT64 MsrGsBase;
    UINT16 Tr;
    UINT16 Ldtr;
    UINT64 DebugControl;
    UINT64 KernelDr7;
    KDESCRIPTOR Idtr;
    KDESCRIPTOR Gdtr;
} SHV_SPECIAL_REGISTERS, *PSHV_SPECIAL_REGISTERS;

typedef struct _SHV_MTRR_RANGE
{
    UINT32 Enabled;
    UINT32 Type;
    UINT64 PhysicalAddressMin;
    UINT64 PhysicalAddressMax;
} SHV_MTRR_RANGE, *PSHV_MTRR_RANGE;

typedef struct _SHV_VP_DATA
{
    union
    {
        DECLSPEC_ALIGN(PAGE_SIZE) UINT8 ShvStackLimit[KERNEL_STACK_SIZE];
        struct
        {
            SHV_SPECIAL_REGISTERS SpecialRegisters;
            CONTEXT ContextFrame;
            UINT64 SystemDirectoryTableBase;
            LARGE_INTEGER MsrData[17];
            SHV_MTRR_RANGE MtrrData[16];
            UINT64 VmxOnPhysicalAddress;
            UINT64 VmcsPhysicalAddress;
            UINT64 MsrBitmapPhysicalAddress;
            UINT64 EptPml4PhysicalAddress;
            UINT32 EptControls;

            //
            // The SVM features (CPUID 8000_000A EDX) and number of ASIDs. The
            // nested page table root is 0 without nested paging
            //
            UINT32 SvmFeatures;
            UINT32 SvmAsidCount;
            UINT64 HostSaveAreaPhysicalAddress;
            UINT64 NptPml4PhysicalAddress;

            //
            // The hypervisor SimpleVisor is nested under, if any
            //
            UINT8 UnderlyingHypervisor;
            UINT8 UnderlyingHypervisorIsVmware;
            UINT8 UnderlyingHypervisorIsHyperV;

            //
            // Whether the guest can use UMONITOR, UMWAIT and TPAUSE. If not
            // WAITPKG is hidden
            //
            UINT8 UserWaitPauseEnabled;

            //
            // Whether VM entry rejected the HLT activity state, as nested
            // Hyper-V does. HLT then resumes the guest right away
            //
            UINT8 HltActivityStateRejected;

            //
            // This processor's bit in the bitmap of virtualized processors
            //
            UINT32 ProcessorIndex;

            //
            // VM-Exits per exit reason, reported to the debugger on unload
            //
            UINT64 ExitCounts[SHV_EXIT_REASON_COUNT];

            //
            // The VM-Exit SimpleVisor could not handle, for the debugger
            //
            UINT64 FatalExitReason;
            UINT64 FatalExitQualification;
            UINT64 FatalGuestPhysicalAddress;
            UINT64 FatalGuestLinearAddress;
            UINT64 FatalGuestRip;

            //
            // Must be last. The hypervisor stack grows down toward it, so an
            // overflow overwrites it first
            //
            UINT64 StackCanary;
        };
    };

    DECLSPEC_ALIGN(PAGE_SIZE) UINT8 MsrBitmap[PAGE_SIZE];

    //
    // The identity map, as EPT tables for VT-x or nested page tables for SVM
    //
    union
    {
        DECLSPEC_ALIGN(PAGE_SIZE) VMX_EPML4E Epml4[PML4E_ENTRY_COUNT];
        DECLSPEC_ALIGN(PAGE_SIZE) SVM_NPT_PML4E Npml4[PML4E_ENTRY_COUNT];
    };
    union
    {
        DECLSPEC_ALIGN(PAGE_SIZE) VMX_PDPTE Epdpt[PDPTE_ENTRY_COUNT];
        DECLSPEC_ALIGN(PAGE_SIZE) SVM_NPT_PDPTE Npdpt[PDPTE_ENTRY_COUNT];
    };
    union
    {
        DECLSPEC_ALIGN(PAGE_SIZE) VMX_LARGE_PDE Epde[PDPTE_ENTRY_COUNT][PDE_ENTRY_COUNT];
        DECLSPEC_ALIGN(PAGE_SIZE) SVM_NPT_LARGE_PDE Npde[PDPTE_ENTRY_COUNT][PDE_ENTRY_COUNT];
    };
    union
    {
        DECLSPEC_ALIGN(PAGE_SIZE) VMX_HUGE_PDPTE EpdptHuge[SHV_EPT_PML4E_COUNT - 1][PDPTE_ENTRY_COUNT];
        DECLSPEC_ALIGN(PAGE_SIZE) SVM_NPT_HUGE_PDPTE NpdptHuge[SHV_EPT_PML4E_COUNT - 1][PDPTE_ENTRY_COUNT];
    };

    //
    // VT-x uses the VMXON region, and SVM the host save area
    //
    union
    {
        DECLSPEC_ALIGN(PAGE_SIZE) VMX_VMCS VmxOn;
        DECLSPEC_ALIGN(PAGE_SIZE) UINT8 HostSaveArea[PAGE_SIZE];
    };
    union
    {
        DECLSPEC_ALIGN(PAGE_SIZE) VMX_VMCS Vmcs;
        DECLSPEC_ALIGN(PAGE_SIZE) SVM_VMCB Vmcb;
    };
} SHV_VP_DATA, *PSHV_VP_DATA;

C_ASSERT(sizeof(SHV_VP_DATA) == (KERNEL_STACK_SIZE + (512 + 5 + SHV_EPT_PML4E_COUNT - 1) * PAGE_SIZE));

VOID
_sldt (
    _In_ UINT16* Ldtr
    );

VOID
_ltr (
    _In_ UINT16 Tr
    );

VOID
_str (
    _In_ UINT16* Tr
    );

VOID
__lgdt (
    _In_ VOID* Gdtr
    );

INT32
ShvLoad (
    VOID
    );

VOID
ShvUnload (
    VOID
    );