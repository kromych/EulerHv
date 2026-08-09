/*++

Copyright (c) Alex Ionescu.  All rights reserved.

Module Name:

    shv.c

Abstract:

    This module implements the Driver Entry/Unload for the Simple Hyper Visor.

Author:

    Alex Ionescu (@aionescu) 16-Mar-2016 - Initial version

Environment:

    Kernel mode only.

--*/

#include "shv.h"

//
// The hardware virtualization backend SimpleVisor runs on
//
const SHV_ARCH* ShvArch;

VOID
ShvUnload (
    VOID
    )
{
    SHV_CALLBACK_CONTEXT callbackContext;
    UINT64 total;
    UINT32 i;

    //
    // Attempt to exit VMX root mode on all logical processors. This will
    // broadcast an interrupt which will execute the callback routine in
    // parallel on the LPs.
    //
    // Note that if SHV is not loaded on any of the LPs, this routine will not
    // perform any work, but will not fail in any way.
    //
    // Each LP adds its VM-Exit counters to the callback context
    //
    __stosb((UINT8*)&callbackContext, 0, sizeof(callbackContext));
    ShvOsRunCallbackOnProcessors(ShvVpUnloadCallback, &callbackContext);
    ShvVpFreeTracking();

    //
    // Report the VM-Exits per exit reason (EXIT_REASON_* in vmx.h, or
    // ShvSvmExitIndex for SVM)
    //
    total = 0;
    for (i = 0; i < SHV_EXIT_REASON_COUNT; i++)
    {
        total += (UINT64)callbackContext.ExitCounts[i];
    }
    ShvOsDebugPrint("The SHV handled %llu VM-Exits\n", total);
    for (i = 0; i < SHV_EXIT_REASON_COUNT - 1; i++)
    {
        if (callbackContext.ExitCounts[i] != 0)
        {
            ShvOsDebugPrint("  Exit reason %d: %llu\n",
                            i, (UINT64)callbackContext.ExitCounts[i]);
        }
    }
    if (callbackContext.ExitCounts[SHV_EXIT_REASON_COUNT - 1] != 0)
    {
        ShvOsDebugPrint("  Exit reasons %d and higher: %llu\n",
                        SHV_EXIT_REASON_COUNT - 1,
                        (UINT64)callbackContext.ExitCounts[SHV_EXIT_REASON_COUNT - 1]);
    }

    //
    // Report the TLB flush hypercalls forwarded to Hyper-V
    //
    if ((callbackContext.TlbHypercallCounts[ShvTlbHypercallFlushSpace] |
         callbackContext.TlbHypercallCounts[ShvTlbHypercallFlushList] |
         callbackContext.TlbHypercallCounts[ShvTlbHypercallFlushSpaceEx] |
         callbackContext.TlbHypercallCounts[ShvTlbHypercallFlushListEx]) != 0)
    {
        ShvOsDebugPrint("The SHV forwarded TLB flush hypercalls: "
                        "space %llu, list %llu, space ex %llu, list ex %llu\n",
                        (UINT64)callbackContext.TlbHypercallCounts[ShvTlbHypercallFlushSpace],
                        (UINT64)callbackContext.TlbHypercallCounts[ShvTlbHypercallFlushList],
                        (UINT64)callbackContext.TlbHypercallCounts[ShvTlbHypercallFlushSpaceEx],
                        (UINT64)callbackContext.TlbHypercallCounts[ShvTlbHypercallFlushListEx]);
    }

    //
    // Report the IPIs observed
    //
    if ((callbackContext.IpiCounts[ShvIpiIcrWrites] |
         callbackContext.IpiCounts[ShvIpiSelfIpiWrites]) != 0)
    {
        ShvOsDebugPrint("The SHV saw IPIs: ICR writes %llu, self IPIs %llu\n",
                        (UINT64)callbackContext.IpiCounts[ShvIpiIcrWrites],
                        (UINT64)callbackContext.IpiCounts[ShvIpiSelfIpiWrites]);
    }

    //
    // Indicate unload
    //
    ShvOsDebugPrint("The SHV has been uninstalled.\n");
}

INT32
ShvLoad (
    VOID
    )
{
    SHV_CALLBACK_CONTEXT callbackContext;
    INT32 status;

    //
    // AMD and Hygon processors implement SVM, the others VT-x
    //
    ShvArch = ShvSvmIsAmd() ? &ShvSvmArch : &ShvVmxArch;

    //
    // Track which logical processors SimpleVisor runs on, by processor number,
    // which can exceed the number of active processors
    //
    status = ShvVpAllocateTracking(ShvOsGetMaximumProcessorCount());
    if (status != SHV_STATUS_SUCCESS)
    {
        return status;
    }

    //
    // Attempt to enter VMX root mode on all logical processors. This will
    // broadcast a DPC interrupt which will execute the callback routine in
    // parallel on the LPs. Send the callback routine the physical address of
    // the PML4 of the system process, which is what this driver entrypoint
    // should be executing in.
    //
    callbackContext.Cr3 = __readcr3();
    callbackContext.FailureStatus = SHV_STATUS_SUCCESS;
    callbackContext.FailedCpu = -1;
    callbackContext.InitCount = 0;
    ShvOsRunCallbackOnProcessors(ShvVpLoadCallback, &callbackContext);

    //
    // Check if all LPs are now hypervised. Return the failure code of at least
    // one of them.
    //
    // Note that each VP is responsible for freeing its VP data on failure.
    // Unload the LPs that did load, as the driver image goes away while their
    // VM-Exits would still run in it.
    //
    if (callbackContext.InitCount != ShvOsGetActiveProcessorCount())
    {
        //
        // A processor that ran the callback and failed says why. Without one,
        // a processor did not run it at all, such as one added meanwhile
        //
        if (callbackContext.FailureStatus == SHV_STATUS_SUCCESS)
        {
            callbackContext.FailureStatus = SHV_STATUS_NOT_PRESENT;
        }
        ShvOsDebugPrint("The SHV failed to initialize (0x%lX) Failed CPU: %d\n",
                        callbackContext.FailureStatus, callbackContext.FailedCpu);
        ShvUnload();
        return callbackContext.FailureStatus;
    }

    //
    // Indicate success.
    //
    ShvOsDebugPrint("The SHV has been installed.\n");
    return SHV_STATUS_SUCCESS;
}
