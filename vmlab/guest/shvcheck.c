/*++

Copyright (c) Roman Kisel 2026.  All rights reserved.

Module Name:

    shvcheck.c

Abstract:

    This user-mode tool runs inside the lab VM and reports, from the guest's
    point of view, the CPUID state that SimpleVisor affects. It prints one
    "key=value" line per check, which Invoke-ShvTest.ps1 parses.

Author:

    kromych (vdso@mailbox.org) 09-May-2026 - Initial version

Environment:

    User mode only, x64.

--*/

#include <windows.h>
#include <intrin.h>
#include <immintrin.h>
#include <stdio.h>
#include <string.h>

static
const char*
TryWaitPkg (
    VOID
    )
{
    volatile UINT64 monitored = 0;

    //
    // Execute TPAUSE, UMONITOR and UMWAIT with a short deadline. If the
    // hypervisor advertises WAITPKG but did not enable "enable user wait and
    // pause", these raise #UD, which surfaces here as an illegal instruction.
    //
    __try
    {
        _tpause(0, __rdtsc() + 10000);
        _umonitor((void*)&monitored);
        _umwait(0, __rdtsc() + 10000);
    }
    __except ((GetExceptionCode() == EXCEPTION_ILLEGAL_INSTRUCTION) ?
              EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        return "ud";
    }

    return "ok";
}

#define MAX_EXIT_COUNTERS 256

static
VOID
ReportExitCounts (
    VOID
    )
{
    static UINT64 counts[MAX_EXIT_COUNTERS];
    GROUP_AFFINITY affinity;
    GROUP_AFFINITY previous;
    INT32 cpuInfo[4];
    UINT32 counters;
    UINT32 processors;
    UINT32 reason;
    UINT64 total;
    WORD group;
    DWORD index;

    //
    // SimpleVisor counts VM-Exits per processor, and a magic CPUID leaf reads
    // the counters of the processor it runs on. So run on each processor in
    // turn, and add up the counters of those that SimpleVisor runs on, as
    // told by the detection leaf. The reads are CPUID exits themselves, and
    // count too.
    //
    counters = 0;
    processors = 0;
    GetThreadGroupAffinity(GetCurrentThread(), &previous);
    for (group = 0; group < GetActiveProcessorGroupCount(); group++)
    {
        for (index = 0; index < GetActiveProcessorCount(group); index++)
        {
            ZeroMemory(&affinity, sizeof(affinity));
            affinity.Group = group;
            affinity.Mask = (KAFFINITY)1 << index;
            if (!SetThreadGroupAffinity(GetCurrentThread(), &affinity, NULL))
            {
                continue;
            }

            __cpuidex(cpuInfo, 0x41414141, 0x44444444);
            if (cpuInfo[0] != ' vhS')
            {
                continue;
            }
            processors++;

            for (reason = 0; reason < MAX_EXIT_COUNTERS; reason++)
            {
                __cpuidex(cpuInfo, 0x41414141, 0x45450000 | reason);
                counters = min((UINT32)cpuInfo[2], MAX_EXIT_COUNTERS);
                if (reason >= counters)
                {
                    break;
                }
                counts[reason] += ((UINT64)(UINT32)cpuInfo[1] << 32) | (UINT32)cpuInfo[0];
            }
        }
    }
    SetThreadGroupAffinity(GetCurrentThread(), &previous, NULL);

    //
    // One line per exit reason (EXIT_REASON_* in SimpleVisor's vmx.h, or
    // ShvSvmExitIndex for SVM) that occurred. The last counter also takes all
    // higher exit reasons.
    //
    total = 0;
    for (reason = 0; reason < counters; reason++)
    {
        total += counts[reason];
    }
    printf("exits.processors=%u\n", processors);
    printf("exits.total=%llu\n", total);
    for (reason = 0; reason < counters; reason++)
    {
        if (counts[reason] != 0)
        {
            printf("exits.%u=%llu\n", reason, counts[reason]);
        }
    }
}

int
main (
    VOID
    )
{
    INT32 cpuInfo[4];
    char vendor[13];
    char iface[5];
    int waitPkg;

    //
    // CPUID 1h: VMX, without which SimpleVisor cannot load on Intel, and the
    // Hypervisor Present-bit, which SimpleVisor always sets
    //
    __cpuid(cpuInfo, 1);
    printf("vmx=%d\n", (cpuInfo[2] >> 5) & 1);
    printf("hypervisor_present=%d\n", (cpuInfo[2] >> 31) & 1);

    //
    // CPUID 80000001h: SVM, without which SimpleVisor cannot load on AMD, and
    // which it keeps reporting while loaded
    //
    __cpuid(cpuInfo, 0x80000000);
    if ((UINT32)cpuInfo[0] >= 0x80000001)
    {
        __cpuid(cpuInfo, 0x80000001);
        printf("svm=%d\n", (cpuInfo[2] >> 2) & 1);
    }
    else
    {
        printf("svm=0\n");
    }

    //
    // CPUID 40000000h/40000001h: the hypervisor vendor and interface. When
    // SimpleVisor is nested, these must still describe the outer hypervisor.
    //
    __cpuid(cpuInfo, 0x40000000);
    memcpy(&vendor[0], &cpuInfo[1], 4);
    memcpy(&vendor[4], &cpuInfo[2], 4);
    memcpy(&vendor[8], &cpuInfo[3], 4);
    vendor[12] = '\0';
    printf("hv_max_leaf=0x%08X\n", cpuInfo[0]);
    printf("hv_vendor=%s\n", vendor);

    __cpuid(cpuInfo, 0x40000001);
    memcpy(iface, &cpuInfo[0], 4);
    iface[4] = '\0';
    printf("hv_interface=%s\n", iface);

    //
    // The magic SimpleVisor detection leaf, answered with ' vhS' only when
    // SimpleVisor is loaded
    //
    __cpuidex(cpuInfo, 0x41414141, 0x44444444);
    printf("simplevisor=%d\n", cpuInfo[0] == ' vhS');

    //
    // The VM-Exits SimpleVisor handled so far, while it is loaded
    //
    if (cpuInfo[0] == ' vhS')
    {
        ReportExitCounts();
    }

    //
    // CPUID 7h.0: WAITPKG must either work, or be hidden
    //
    __cpuidex(cpuInfo, 7, 0);
    waitPkg = (cpuInfo[2] >> 5) & 1;
    printf("waitpkg=%d\n", waitPkg);
    printf("waitpkg_exec=%s\n", waitPkg ? TryWaitPkg() : "skipped");
    return 0;
}
