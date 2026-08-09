/*++

Copyright (c) Roman Kisel 2026.  All rights reserved.

Module Name:

    eulercheck.c

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

#include "..\..\inc\shvcheck.h"

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

//
// The TLB flush hypercalls SimpleVisor counts, in the order of its
// ShvTlbHypercall
//
static const char* TlbHypercallNames[] =
{
    "flush_space",
    "flush_list",
    "flush_space_ex",
    "flush_list_ex",
};

#define TLB_HYPERCALL_COUNTERS (sizeof(TlbHypercallNames) / sizeof(TlbHypercallNames[0]))

//
// The IPIs SimpleVisor counts, in the order of its ShvIpiCounter
//
static const char* IpiCounterNames[] =
{
    "icr_writes",
    "self_ipis",
    "flush_sent",
    "flush_received",
    "nmis_injected",
};

#define IPI_COUNTERS (sizeof(IpiCounterNames) / sizeof(IpiCounterNames[0]))

static
VOID
ReportExitCounts (
    VOID
    )
{
    static UINT64 counts[MAX_EXIT_COUNTERS];
    static UINT64 tlbCounts[TLB_HYPERCALL_COUNTERS];
    static UINT64 ipiCounts[IPI_COUNTERS];
    GROUP_AFFINITY affinity;
    GROUP_AFFINITY previous;
    INT32 cpuInfo[4];
    UINT32 counters;
    UINT32 tlbCounters;
    UINT32 ipiCounters;
    UINT32 processors;
    UINT32 reason;
    UINT64 total;
    int x2Apic;
    WORD group;
    DWORD index;

    //
    // SimpleVisor counts VM-Exits per processor, and a magic CPUID leaf reads
    // the counters of the processor it runs on. So run on each processor in
    // turn, and add up the counters of those that SimpleVisor runs on, as
    // told by the detection leaf. The reads are CPUID exits themselves, and
    // count too. Other leaves read the TLB flush hypercalls it forwarded to
    // Hyper-V and the IPIs it saw the same way.
    //
    counters = 0;
    tlbCounters = 0;
    ipiCounters = 0;
    processors = 0;
    x2Apic = 1;
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

            __cpuidex(cpuInfo, SHV_CPUID_LEAF, SHV_CPUID_DETECT);
            if (cpuInfo[0] != SHV_CPUID_SIGNATURE)
            {
                continue;
            }
            processors++;

            //
            // The detection leaf reports x2APIC mode in EBX bit 0, which
            // every virtualized processor must have for the flush IPIs
            //
            x2Apic = x2Apic && ((cpuInfo[1] & 1) != 0);

            for (reason = 0; reason < MAX_EXIT_COUNTERS; reason++)
            {
                __cpuidex(cpuInfo, SHV_CPUID_LEAF, SHV_CPUID_EXIT_COUNTS | reason);
                counters = min((UINT32)cpuInfo[2], MAX_EXIT_COUNTERS);
                if (reason >= counters)
                {
                    break;
                }
                counts[reason] += ((UINT64)(UINT32)cpuInfo[1] << 32) | (UINT32)cpuInfo[0];
            }

            for (reason = 0; reason < TLB_HYPERCALL_COUNTERS; reason++)
            {
                __cpuidex(cpuInfo, SHV_CPUID_LEAF, SHV_CPUID_TLB_COUNTS | reason);
                tlbCounters = min((UINT32)cpuInfo[2], TLB_HYPERCALL_COUNTERS);
                if (reason >= tlbCounters)
                {
                    break;
                }
                tlbCounts[reason] += ((UINT64)(UINT32)cpuInfo[1] << 32) | (UINT32)cpuInfo[0];
            }

            for (reason = 0; reason < IPI_COUNTERS; reason++)
            {
                __cpuidex(cpuInfo, SHV_CPUID_LEAF, SHV_CPUID_IPI_COUNTS | reason);
                ipiCounters = min((UINT32)cpuInfo[2], IPI_COUNTERS);
                if (reason >= ipiCounters)
                {
                    break;
                }
                ipiCounts[reason] += ((UINT64)(UINT32)cpuInfo[1] << 32) | (UINT32)cpuInfo[0];
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
    printf("x2apic=%d\n", x2Apic);
    printf("exits.total=%llu\n", total);
    for (reason = 0; reason < counters; reason++)
    {
        if (counts[reason] != 0)
        {
            printf("exits.%u=%llu\n", reason, counts[reason]);
        }
    }

    //
    // One line per TLB flush hypercall, as Hyper-V recommends them to the
    // guest only on some configurations
    //
    for (reason = 0; reason < tlbCounters; reason++)
    {
        printf("tlbcalls.%s=%llu\n", TlbHypercallNames[reason], tlbCounts[reason]);
    }

    //
    // One line per IPI counter
    //
    for (reason = 0; reason < ipiCounters; reason++)
    {
        printf("ipis.%s=%llu\n", IpiCounterNames[reason], ipiCounts[reason]);
    }
}

//
// The NMI injection command: "eulercheck nmi" has SimpleVisor inject an NMI
// into this processor, which the guest's NMI handler takes. SimpleVisor
// halts on the other NMIs, so this is the only way the guest sees one
//
static
INT32
InjectNmi (
    VOID
    )
{
    INT32 cpuInfo[4];

    __cpuidex(cpuInfo, SHV_CPUID_LEAF, SHV_CPUID_DETECT);
    if (cpuInfo[0] != SHV_CPUID_SIGNATURE)
    {
        printf("nmi=not_loaded\n");
        return 1;
    }

    //
    // The NMI delivers when the CPUID completes
    //
    __cpuidex(cpuInfo, SHV_CPUID_LEAF, SHV_CPUID_NMI);
    printf("nmi=sent\n");
    return 0;
}

int
main (
    INT32 argc,
    CHAR* argv[]
    )
{
    INT32 cpuInfo[4];
    char vendor[13];
    char iface[5];
    int waitPkg;

    if ((argc > 1) && (strcmp(argv[1], "nmi") == 0))
    {
        return InjectNmi();
    }

    //
    // CPUID 1h: VMX, without which SimpleVisor cannot load on Intel, the
    // Hypervisor Present-bit, which SimpleVisor always sets, and x2APIC
    // support (ECX bit 21), which the lab requires of the platform, as the
    // flush IPIs need it. The full registers let the host scripts build a
    // CPUID override for OpenVMM that forces x2APIC and keeps the rest
    //
    __cpuid(cpuInfo, 1);
    printf("vmx=%d\n", (cpuInfo[2] >> 5) & 1);
    printf("hypervisor_present=%d\n", (cpuInfo[2] >> 31) & 1);
    printf("x2apic_cpuid=%d\n", (cpuInfo[2] >> 21) & 1);
    printf("cpuid1_eax=0x%08X\n", (UINT32)cpuInfo[0]);
    printf("cpuid1_ebx=0x%08X\n", (UINT32)cpuInfo[1]);
    printf("cpuid1_ecx=0x%08X\n", (UINT32)cpuInfo[2]);
    printf("cpuid1_edx=0x%08X\n", (UINT32)cpuInfo[3]);

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
    // CPUID 40000003h under Hyper-V: the partition privileges, which decide
    // whether SimpleVisor idles in the guest idle MSR (EAX bit 10). CPUID
    // 40000004h: the recommendations, of which the one for hypercalls to
    // switch address spaces (EAX bit 0) used to keep SimpleVisor from
    // loading, which it now handles by emulating the switch. CPUID
    // 4000000Ah: the nested features, which decide whether it flushes the
    // guest's mappings on every processor with Hyper-V's hypercall (EAX bit
    // 18) or with its own IPIs
    //
    if (strcmp(iface, "Hv#1") == 0)
    {
        __cpuid(cpuInfo, 0x40000003);
        printf("hv_privileges=0x%08X\n", cpuInfo[0]);
        __cpuid(cpuInfo, 0x40000004);
        printf("hv_enlightenments=0x%08X\n", cpuInfo[0]);
        __cpuid(cpuInfo, 0x40000000);
        if ((UINT32)cpuInfo[0] >= 0x4000000A)
        {
            __cpuid(cpuInfo, 0x4000000A);
            printf("hv_nested_features=0x%08X\n", cpuInfo[0]);
        }
    }

    //
    // The magic SimpleVisor detection leaf, answered with the SimpleVisor
    // signature only when SimpleVisor is loaded
    //
    __cpuidex(cpuInfo, SHV_CPUID_LEAF, SHV_CPUID_DETECT);
    printf("simplevisor=%d\n", cpuInfo[0] == SHV_CPUID_SIGNATURE);

    //
    // The VM-Exits SimpleVisor handled so far, while it is loaded
    //
    if (cpuInfo[0] == SHV_CPUID_SIGNATURE)
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
