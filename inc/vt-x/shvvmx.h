/*++

Copyright (c) Alex Ionescu.  All rights reserved.

Header Name:

    shvvmx.h

Abstract:

    This header defines the routines shared between the Intel VMX
    (Vanderpool/VT-x) modules, which the rest of SimpleVisor reaches through
    ShvVmxArch.

Author:

    Alex Ionescu (@aionescu) 16-Mar-2016 - Initial version

Environment:

    Kernel mode only.

--*/

#pragma once

#include "shv.h"

VOID
ShvVmxEntry (
    VOID
    );

VOID
ShvVmxHypercall (
    _Inout_ PCONTEXT Context
    );

SHV_HYPERCALL ShvVmxOwnHypercall;

VOID
ShvVmxInvept (
    _In_ UINT64 Type,
    _In_ UINT64* Descriptor
    );

VOID
ShvVmxInvvpid (
    _In_ UINT64 Type,
    _In_ UINT64* Descriptor
    );

INT32
ShvVmxLaunch (
    _In_ PSHV_VP_DATA VpData
    );

VOID
ShvVmxOff (
    _In_ PSHV_VP_DATA VpData
    );

VOID
ShvVmxHandleExit (
    _In_ PSHV_VP_STATE VpState
    );

VOID
ShvVmxFlushGuestMappings (
    _In_ PSHV_VP_DATA VpData
    );

VOID
ShvVmxTakeHeldNmi (
    _In_ PSHV_VP_DATA VpData
    );

UINT8
ShvVmxReflectNmi (
    _In_ PSHV_VP_DATA VpData
    );
