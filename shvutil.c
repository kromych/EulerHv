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

VOID
ShvUtilMtrrInitialize (
    _In_ PSHV_VP_DATA VpData
    )
{
    UINT32 i;
    MTRR_CAPABILITIES mtrrCapabilities;
    MTRR_VARIABLE_BASE mtrrBase;
    MTRR_VARIABLE_MASK mtrrMask;
    UINT64 mtrrDefault;
    unsigned long bit;

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
            // Compute the length
            //
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
