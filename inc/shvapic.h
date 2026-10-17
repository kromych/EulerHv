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
// to make it do so. SimpleVisor's flush IPIs need the x2APIC MSRs, or else
// Hyper-V's synthetic ICR
//
#define APIC_BASE                   0x1B
#define APIC_BASE_X2APIC            (1ULL << 10)

//
// The x2APIC MSR numbers. The guest runs in x2APIC mode, where the local APIC
// registers are MSRs at 0x800 plus the register offset
//
#define APIC_ID                     0x802
#define APIC_ICR                    0x830
#define APIC_SELF_IPI               0x83F

//
// The x2APIC ICR format. The destination APIC ID occupies bits 63:32, the
// delivery mode bits 10:8 and the level bit 14, and destination shorthand 0
// (none) leaves bits 19:18 clear
//
#define APIC_ICR_DESTINATION_SHIFT  32
#define APIC_ICR_DELIVERY_NMI       (4ULL << 8)
#define APIC_ICR_LEVEL_ASSERT       (1ULL << 14)

//
// Hyper-V's synthetic ICR, one of its APIC access MSRs, which works in xAPIC
// mode as well. It takes the two halves of the xAPIC ICR, so the destination
// xAPIC ID occupies bits 63:56, and the other bits are as above
//
#define HV_X64_MSR_ICR              0x40000071
#define HV_ICR_DESTINATION_SHIFT    56

//
// SimpleVisor flushes the guest's mappings on another processor with an NMI
// IPI. It is never delivered to the guest: the receiving processor exits on
// the NMI, or takes it through SimpleVisor's NMI gate in root mode, and
// flushes there. Unlike an interrupt, an NMI is not held off by the guest's
// IF or TPR, and needs no EOI, so the guest's interrupts reach it directly
//
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
    // NMIs injected on the explicit command of vmlab\guest\eulercheck.c, and
    // NMIs that are not flush IPIs, which SimpleVisor passes on to the guest,
    // such as those Windows freezes the processors with for the debugger.
    // SVM halts for the debugger on those instead
    //
    ShvIpiNmisInjected,
    ShvIpiNmisReflected,
    ShvIpiCount
} ShvIpiCounter;
