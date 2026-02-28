/*++

Module Name:

    shvcheck.c

Abstract:

    This user-mode tool runs inside the lab VM and reports, from the guest's
    point of view, the CPUID state that SimpleVisor affects. It prints one
    "key=value" line per check, which Invoke-ShvTest.ps1 parses.

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
    // CPUID 1h: VMX, without which SimpleVisor cannot load at all, and the
    // Hypervisor Present-bit, which SimpleVisor always sets
    //
    __cpuid(cpuInfo, 1);
    printf("vmx=%d\n", (cpuInfo[2] >> 5) & 1);
    printf("hypervisor_present=%d\n", (cpuInfo[2] >> 31) & 1);

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
    // CPUID 7h.0: WAITPKG must either work, or be hidden
    //
    __cpuidex(cpuInfo, 7, 0);
    waitPkg = (cpuInfo[2] >> 5) & 1;
    printf("waitpkg=%d\n", waitPkg);
    printf("waitpkg_exec=%s\n", waitPkg ? TryWaitPkg() : "skipped");
    return 0;
}
