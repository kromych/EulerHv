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
// IA32_APIC_BASE and its x2APIC enable bit. Windows enables x2APIC at boot
// when the processor offers it, and the lab sets the x2apicpolicy BCD option
// to make it do so, as SimpleVisor's flush IPIs need the x2APIC MSRs
//
#define APIC_BASE                   0x1B
#define APIC_BASE_X2APIC            (1ULL << 10)

//
// The x2APIC MSR numbers. The guest runs in x2APIC mode, where the local APIC
// registers are MSRs at 0x800 plus the register offset
//
#define APIC_ID                     0x802
#define APIC_EOI                    0x80B
#define APIC_ICR                    0x830
#define APIC_SELF_IPI               0x83F

//
// The x2APIC ICR format. The destination APIC ID occupies bits 63:32 and the
// vector bits 7:0. Delivery mode 0 (fixed) and destination shorthand 0 (none)
// leave the rest clear
//
#define APIC_ICR_DESTINATION_SHIFT  32

//
// The vector of the IPI that SimpleVisor sends when it flushes the guest's
// mappings on another processor. It is never delivered to the guest: the
// receiving processor exits on the interrupt and flushes in root mode.
// Vector 0xFE is the highest priority class, so the local APIC accepts it
// regardless of the guest's TPR
//
#define SHV_IPI_FLUSH_VECTOR        0xFE

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

    //
    // SimpleVisor's own flush IPIs, sent and received
    //
    ShvIpiFlushIpisSent,
    ShvIpiFlushIpisReceived,

    //
    // NMIs injected on the explicit command of vmlab\guest\eulercheck.c,
    // the only NMIs the guest sees. Any other NMI halts for the debugger
    //
    ShvIpiNmisInjected,
    ShvIpiCount
} ShvIpiCounter;
