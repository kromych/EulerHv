/*++

Copyright (c) Alex Ionescu.  All rights reserved.

Module Name:

    shvutil.c

Abstract:

    This module implements utility functions for the Simple Hyper Visor.

Author:

    Alex Ionescu (@aionescu) 16-Mar-2016 - Initial version

Environment:

    Kernel mode only.

--*/

#include "shv.h"

VOID
ShvUtilConvertGdtEntry (
    _In_ VOID* GdtBase,
    _In_ UINT16 Selector,
    _Out_ PVMX_GDTENTRY64 VmxGdtEntry
    )
{
    PKGDTENTRY64 gdtEntry;

    //
    // Reject LDT or NULL entries
    //
    if ((Selector == 0) ||
        (Selector & SELECTOR_TABLE_INDEX) != 0)
    {
        VmxGdtEntry->Limit = VmxGdtEntry->AccessRights = 0;
        VmxGdtEntry->Base = 0;
        VmxGdtEntry->Selector = 0;
        VmxGdtEntry->Bits.Unusable = TRUE;
        return;
    }

    //
    // Read the GDT entry at the given selector, masking out the RPL bits.
    //
    gdtEntry = (PKGDTENTRY64)((uintptr_t)GdtBase + (Selector & ~RPL_MASK));

    //
    // Write the selector directly 
    //
    VmxGdtEntry->Selector = Selector;

    //
    // Use the LSL intrinsic to read the segment limit
    //
    VmxGdtEntry->Limit = __segmentlimit(Selector);

    //
    // Build the full 64-bit effective address, keeping in mind that only when
    // the System bit is unset, should this be done.
    //
    // NOTE: The Windows definition of KGDTENTRY64 is WRONG. The "System" field
    // is incorrectly defined at the position of where the AVL bit should be.
    // The actual location of the SYSTEM bit is encoded as the highest bit in
    // the "Type" field.
    //
    VmxGdtEntry->Base = ((gdtEntry->Bytes.BaseHigh << 24) |
                         (gdtEntry->Bytes.BaseMiddle << 16) |
                         (gdtEntry->BaseLow)) & 0xFFFFFFFF;
    VmxGdtEntry->Base |= ((gdtEntry->Bits.Type & 0x10) == 0) ?
                         ((uintptr_t)gdtEntry->BaseUpper << 32) : 0;

    //
    // Load the access rights
    //
    VmxGdtEntry->AccessRights = 0;
    VmxGdtEntry->Bytes.Flags1 = gdtEntry->Bytes.Flags1;
    VmxGdtEntry->Bytes.Flags2 = gdtEntry->Bytes.Flags2;

    //
    // Finally, handle the VMX-specific bits
    //
    VmxGdtEntry->Bits.Reserved = 0;
    VmxGdtEntry->Bits.Unusable = !gdtEntry->Bits.Present;
}

UINT32
ShvUtilAdjustMsr (
    _In_ LARGE_INTEGER ControlValue,
    _In_ UINT32 DesiredValue
    )
{
    //
    // VMX feature/capability MSRs encode the "must be 0" bits in the high word
    // of their value, and the "must be 1" bits in the low word of their value.
    // Adjust any requested capability/feature based on these requirements.
    //
    DesiredValue &= ControlValue.HighPart;
    DesiredValue |= ControlValue.LowPart;
    return DesiredValue;
}

//
// The fixed-range MTRRs, and the 4KB pages below 1MB they cover. The first
// covers 64KB per type, the next two 16KB and the rest 4KB
//
typedef struct _SHV_MTRR_FIXED_RANGE
{
    UINT32 Msr;
    UINT32 FirstPage;
    UINT32 PagesPerType;
} SHV_MTRR_FIXED_RANGE;

static const SHV_MTRR_FIXED_RANGE ShvUtilMtrrFixedRanges[] =
{
    { MTRR_MSR_FIX64K_00000, 0x00, 16 },
    { MTRR_MSR_FIX16K_80000, 0x80, 4 },
    { MTRR_MSR_FIX16K_A0000, 0xA0, 4 },
    { MTRR_MSR_FIX4K_C0000 + 0, 0xC0, 1 },
    { MTRR_MSR_FIX4K_C0000 + 1, 0xC8, 1 },
    { MTRR_MSR_FIX4K_C0000 + 2, 0xD0, 1 },
    { MTRR_MSR_FIX4K_C0000 + 3, 0xD8, 1 },
    { MTRR_MSR_FIX4K_C0000 + 4, 0xE0, 1 },
    { MTRR_MSR_FIX4K_C0000 + 5, 0xE8, 1 },
    { MTRR_MSR_FIX4K_C0000 + 6, 0xF0, 1 },
    { MTRR_MSR_FIX4K_C0000 + 7, 0xF8, 1 },
};

VOID
ShvUtilFlushTlb (
    VOID
    )
{
    INT32 cpuInfo[4];
    UINT64 descriptor[2];
    UINT64 cr4;

    //
    // The guest's INVLPG, INVPCID and MOV CR3 only invalidated its own VPID
    // or ASID, so the mappings the OS cached before the load can be stale.
    // Invalidate every PCID, including global pages, before it runs again
    //
    __cpuid(cpuInfo, 0);
    if ((UINT32)cpuInfo[0] >= 7)
    {
        __cpuidex(cpuInfo, 7, 0);
        if ((cpuInfo[1] & CPUID_FEATURES_EBX_INVPCID) != 0)
        {
            descriptor[0] = descriptor[1] = 0;
            _invpcid(2, descriptor);
            return;
        }
    }

    //
    // Without INVPCID, changing CR4.PGE invalidates every PCID as well
    //
    cr4 = __readcr4();
    __writecr4(cr4 ^ CR4_PGE);
    __writecr4(cr4);
}

static
VOID
ShvUtilSetInterruptGate (
    _Out_ PKIDTENTRY64 Gate,
    _In_ UINT16 Selector,
    _In_ uintptr_t Handler
    )
{
    //
    // A kernel interrupt gate on the current stack, which clears IF on entry
    //
    Gate->OffsetLow = (UINT16)Handler;
    Gate->Selector = Selector;
    Gate->IstIndex = 0;
    Gate->Reserved0 = 0;
    Gate->Type = AMD64_INTERRUPT_GATE;
    Gate->Dpl = 0;
    Gate->Present = 1;
    Gate->OffsetMiddle = (UINT16)(Handler >> 16);
    Gate->OffsetHigh = (UINT32)(Handler >> 32);
    Gate->Reserved1 = 0;
}

VOID
ShvUtilBuildIdleIdt (
    _In_ PSHV_VP_DATA VpData
    )
{
    PKIDTENTRY64 idt = VpData->IdleIdt;
    UINT16 selector = VpData->ContextFrame.SegCs & ~RPL_MASK;
    UINT32 vector;

    //
    // Build the IDT for idling when the guest halts. NMIs and exceptions keep
    // the OS's gates, as with the host IDT, and interrupts go to the idle
    // stubs
    //
    __movsb((UINT8*)idt,
            VpData->SpecialRegisters.Idtr.Base,
            IDT_FIRST_INTERRUPT * sizeof(KIDTENTRY64));
    for (vector = IDT_FIRST_INTERRUPT; vector < IDT_ENTRY_COUNT; vector++)
    {
        ShvUtilSetInterruptGate(&idt[vector],
                                selector,
                                (uintptr_t)ShvIdleStubs +
                                (vector - IDT_FIRST_INTERRUPT) * SHV_IDLE_STUB_SIZE);
    }

    //
    // ShvUtilAccessMsr loads this IDT as well. Its #GP gate fails the MSR
    // access, as nothing else raises #GP while the IDT is loaded
    //
    ShvUtilSetInterruptGate(&idt[IDT_GENERAL_PROTECTION],
                            selector,
                            (uintptr_t)ShvMsrFault);
}

UINT32
ShvUtilIdle (
    _In_ PSHV_VP_DATA VpData
    )
{
    KDESCRIPTOR hostIdtr;
    KDESCRIPTOR idleIdtr;
    UINT32 vector;

    //
    // Halt until an interrupt comes in, and return its vector for the guest,
    // or 0 if the processor woke up without one, such as after an NMI.
    //
    // Under Hyper-V, read its guest idle MSR when allowed. It returns once an
    // interrupt is pending, even with interrupts disabled, so the interrupt
    // stays pending in the local APIC and the guest takes it once it runs
    //
    if (VpData->HyperVGuestIdle != FALSE)
    {
        __readmsr(HYPERV_MSR_GUEST_IDLE);
        return 0;
    }

    //
    // Otherwise halt. The host would deliver interrupts through the OS's IDT,
    // so load the idle IDT, which takes the interrupt in a stub. CR8 is the
    // guest's, so the local APIC only delivers interrupts the guest would take
    //
    idleIdtr.Base = VpData->IdleIdt;
    idleIdtr.Limit = sizeof(VpData->IdleIdt) - 1;
    __sidt(&hostIdtr.Limit);
    __lidt(&idleIdtr.Limit);
    vector = ShvIdleWaitForInterrupt();
    __lidt(&hostIdtr.Limit);
    return vector;
}

UINT8
ShvUtilAccessMsr (
    _In_ PSHV_VP_DATA VpData,
    _In_ UINT32 Msr,
    _In_ UINT8 Write,
    _Inout_ UINT64* Value
    )
{
    KDESCRIPTOR hostIdtr;
    KDESCRIPTOR idleIdtr;
    UINT8 success;

    //
    // Read or write an MSR that exited, from root mode, and return FALSE if it
    // raised #GP, which the guest then gets. In root mode the #GP would go to
    // the OS's IDT, so load the idle IDT, whose #GP gate fails the access.
    // Interrupts are held off in root mode, and NMIs and machine checks keep
    // the OS's gates
    //
    idleIdtr.Base = VpData->IdleIdt;
    idleIdtr.Limit = sizeof(VpData->IdleIdt) - 1;
    __sidt(&hostIdtr.Limit);
    __lidt(&idleIdtr.Limit);
    success = (Write != FALSE) ? ShvMsrWrite(Msr, *Value) : ShvMsrRead(Msr, Value);
    __lidt(&hostIdtr.Limit);
    return success;
}

UINT8
ShvUtilCountHypercall (
    _In_ PSHV_VP_DATA VpData,
    _In_ UINT64 HypercallInput
    )
{
    //
    // Count the TLB flush hypercalls by the call code in the low 16 bits of
    // the input value, and return whether this is one. A rep hypercall that
    // Hyper-V returns early to is issued again from its rep start index, and
    // counts again
    //
    switch (HypercallInput & HYPERV_HYPERCALL_CODE_MASK)
    {
    case HvCallFlushVirtualAddressSpace:
        VpData->TlbHypercallCounts[ShvTlbHypercallFlushSpace]++;
        return TRUE;
    case HvCallFlushVirtualAddressList:
        VpData->TlbHypercallCounts[ShvTlbHypercallFlushList]++;
        return TRUE;
    case HvCallFlushVirtualAddressSpaceEx:
        VpData->TlbHypercallCounts[ShvTlbHypercallFlushSpaceEx]++;
        return TRUE;
    case HvCallFlushVirtualAddressListEx:
        VpData->TlbHypercallCounts[ShvTlbHypercallFlushListEx]++;
        return TRUE;
    }
    return FALSE;
}

VOID
ShvUtilMtrrInitialize (
    _In_ PSHV_VP_DATA VpData
    )
{
    UINT32 i, j;
    MTRR_CAPABILITIES mtrrCapabilities;
    MTRR_VARIABLE_BASE mtrrBase;
    MTRR_VARIABLE_MASK mtrrMask;
    UINT64 mtrrDefault;
    UINT64 fixedTypes;
    unsigned long bit;
    INT32 cpuInfo[4];

    //
    // Without MTRRs only the PAT decides the memory type, which WB in the
    // second level tables leaves in effect. Their MSRs would raise #GP
    //
    __cpuid(cpuInfo, 1);
    if ((cpuInfo[3] & CPUID_FEATURES_EDX_MTRR) == 0)
    {
        VpData->MtrrDefaultType = MTRR_TYPE_WB;
        return;
    }

    //
    // The default type applies outside the variable MTRRs. With the MTRRs
    // disabled all memory is UC, and the variable MTRRs do not apply
    //
    mtrrDefault = __readmsr(MTRR_MSR_DEFAULT);
    if ((mtrrDefault & MTRR_DEFAULT_ENABLE) == 0)
    {
        VpData->MtrrDefaultType = MTRR_TYPE_UC;
        return;
    }
    VpData->MtrrDefaultType = (UINT32)(mtrrDefault & MTRR_DEFAULT_TYPE_MASK);

    //
    // Read the capabilities mask
    //
    mtrrCapabilities.AsUlonglong = __readmsr(MTRR_MSR_CAPABILITIES);

    //
    // Each fixed-range MTRR holds the types of 8 consecutive ranges, one per
    // byte. Spread them over the 4KB pages below 1MB
    //
    if ((mtrrCapabilities.FixedSupported != FALSE) &&
        ((mtrrDefault & MTRR_DEFAULT_FIXED_ENABLE) != 0))
    {
        for (i = 0; i < sizeof(ShvUtilMtrrFixedRanges) / sizeof(ShvUtilMtrrFixedRanges[0]); i++)
        {
            fixedTypes = __readmsr(ShvUtilMtrrFixedRanges[i].Msr);
            for (j = 0; j < 8 * ShvUtilMtrrFixedRanges[i].PagesPerType; j++)
            {
                VpData->MtrrFixedTypes[ShvUtilMtrrFixedRanges[i].FirstPage + j] =
                    (UINT8)(fixedTypes >> ((j / ShvUtilMtrrFixedRanges[i].PagesPerType) * 8));
            }
        }
        VpData->MtrrFixedEnabled = TRUE;
    }

    //
    // Iterate over each variable MTRR, up to as many as we can track
    //
    for (i = 0; (i < mtrrCapabilities.VarCnt) &&
                (i < sizeof(VpData->MtrrData) / sizeof(VpData->MtrrData[0])); i++)
    {
        //
        // Capture the value
        //
        mtrrBase.AsUlonglong = __readmsr(MTRR_MSR_VARIABLE_BASE + i * 2);
        mtrrMask.AsUlonglong = __readmsr(MTRR_MSR_VARIABLE_MASK + i * 2);

        //
        // Check if the MTRR is enabled
        //
        VpData->MtrrData[i].Type = (UINT32)mtrrBase.Type;
        VpData->MtrrData[i].Enabled = (UINT32)mtrrMask.Enabled;
        if (VpData->MtrrData[i].Enabled != FALSE)
        {
            //
            // Set the base
            //
            VpData->MtrrData[i].PhysicalAddressMin = mtrrBase.PhysBase *
                                                     MTRR_PAGE_SIZE;

            //
            // Compute the length. A mask without bits matches every address
            //
            if (mtrrMask.PhysMask == 0)
            {
                VpData->MtrrData[i].PhysicalAddressMin = 0;
                VpData->MtrrData[i].PhysicalAddressMax = ~0ULL;
                continue;
            }
            _BitScanForward64(&bit, mtrrMask.PhysMask * MTRR_PAGE_SIZE);
            VpData->MtrrData[i].PhysicalAddressMax = VpData->MtrrData[i].
                                                     PhysicalAddressMin +
                                                     (1ULL << bit) - 1;
        }
    }
}

static
UINT32
ShvUtilMtrrCombineMemoryTypes (
    _In_ UINT32 MemoryType,
    _In_ UINT32 OtherMemoryType
    )
{
    //
    // Overlapping MTRRs follow the precedence of the SDM: UC wins, WT wins over
    // WB, and other mixes are undefined, so use UC, which is always safe.
    // MTRR_TYPE_MAX stands for no type yet
    //
    if ((MemoryType == MTRR_TYPE_MAX) || (MemoryType == OtherMemoryType))
    {
        return OtherMemoryType;
    }
    if (((MemoryType == MTRR_TYPE_WT) && (OtherMemoryType == MTRR_TYPE_WB)) ||
        ((MemoryType == MTRR_TYPE_WB) && (OtherMemoryType == MTRR_TYPE_WT)))
    {
        return MTRR_TYPE_WT;
    }
    return MTRR_TYPE_UC;
}

UINT32
ShvUtilMtrrGetMemoryType (
    _In_ PSHV_VP_DATA VpData,
    _In_ UINT64 PageAddress,
    _In_ UINT64 PageSize
    )
{
    UINT32 i;
    UINT32 memoryType;
    UINT64 pageEnd;
    UINT8 covered;

    //
    // A large page has a single memory type, so combine the types of every
    // MTRR it touches. Unless one of them covers the whole page, part of it
    // may get the default type, so combine that too
    //
    memoryType = MTRR_TYPE_MAX;
    covered = FALSE;
    pageEnd = PageAddress + PageSize - 1;

    //
    // Below 1MB the fixed-range MTRRs take precedence, so combine their types
    // and look up the rest of the page in the variable MTRRs
    //
    if ((VpData->MtrrFixedEnabled != FALSE) && (PageAddress < _1MB))
    {
        for (i = (UINT32)(PageAddress / PAGE_SIZE);
             (i < sizeof(VpData->MtrrFixedTypes)) && (i <= pageEnd / PAGE_SIZE);
             i++)
        {
            memoryType = ShvUtilMtrrCombineMemoryTypes(memoryType,
                                                       VpData->MtrrFixedTypes[i]);
        }
        if (pageEnd < _1MB)
        {
            return memoryType;
        }
        PageAddress = _1MB;
    }
    for (i = 0; i < sizeof(VpData->MtrrData) / sizeof(VpData->MtrrData[0]); i++)
    {
        //
        // Skip disabled ranges, and ranges that don't touch this page
        //
        if ((VpData->MtrrData[i].Enabled == FALSE) ||
            (VpData->MtrrData[i].PhysicalAddressMax < PageAddress) ||
            (VpData->MtrrData[i].PhysicalAddressMin > pageEnd))
        {
            continue;
        }

        if ((VpData->MtrrData[i].PhysicalAddressMin <= PageAddress) &&
            (VpData->MtrrData[i].PhysicalAddressMax >= pageEnd))
        {
            covered = TRUE;
        }
        memoryType = ShvUtilMtrrCombineMemoryTypes(memoryType,
                                                   VpData->MtrrData[i].Type);
    }

    if (covered == FALSE)
    {
        memoryType = ShvUtilMtrrCombineMemoryTypes(memoryType,
                                                   VpData->MtrrDefaultType);
    }
    return memoryType;
}
