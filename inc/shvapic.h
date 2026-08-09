/*++

Copyright (c) Roman Kisel 2026.  All rights reserved.

Header Name:

    shvapic.h

Abstract:

    This header defines the local APIC state and controls that SimpleVisor
    uses for its IPIs and NMIs.

Author:

    kromych (vdso@mailbox.org) 29-Oct-2026 - Initial version

Environment:

    Kernel mode only.

--*/

#pragma once

//
// The x2APIC MSR numbers. The guest runs in x2APIC mode, where the local APIC
// registers are MSRs at 0x800 plus the register offset
//
#define APIC_ICR                    0x830
#define APIC_SELF_IPI               0x83F

//
// The IPIs SimpleVisor counts per processor, reported on unload and read by
// vmlab\guest\eulercheck.c
//
typedef enum _ShvIpiCounter
{
    //
    // Guest writes to the ICR and the self IPI register, which SimpleVisor
    // intercepts and passes on. The ICR does not say what the IPI is for, as
    // the OS carries that in its own per-processor IPI mailbox, so SimpleVisor
    // cannot tell the TLB flushes from the other IPIs
    //
    ShvIpiIcrWrites,
    ShvIpiSelfIpiWrites,
    ShvIpiCount
} ShvIpiCounter;
