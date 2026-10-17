/*++

Copyright (c) Roman Kisel 2026.  All rights reserved.

Header Name:

    shvcheck.h

Abstract:

    This header defines the magic CPUID interface between SimpleVisor and
    the guest checker, vmlab\guest\eulercheck.c. Both share it, so it must
    compile in kernel mode and in user mode.

Author:

    kromych (vdso@mailbox.org) 29-Oct-2026 - Initial version

Environment:

    Kernel mode and user mode.

--*/

#pragma once

//
// The base value of the magic CPUID leaf, in EAX. The ECX values below pick
// the call
//
#define SHV_CPUID_LEAF                 0x41414141

//
// The ECX values of the magic CPUID calls
//
#define SHV_CPUID_UNLOAD               0x42424242
#define SHV_CPUID_DETECT               0x44444444
#define SHV_CPUID_EXIT_COUNTS          0x45450000
#define SHV_CPUID_TLB_COUNTS           0x46460000
#define SHV_CPUID_IPI_COUNTS           0x47470000

//
// The EBX bits of the detection call: whether this processor's local APIC is
// in x2APIC mode, and whether SimpleVisor's flush IPIs reach this processor
//
#define SHV_CPUID_DETECT_X2APIC        0x00000001
#define SHV_CPUID_DETECT_FLUSH_IPIS    0x00000002

//
// The command to inject an NMI into this processor, which the guest's NMI
// handler takes. SimpleVisor drops the other NMIs, so only this command
// delivers one
//
#define SHV_CPUID_NMI                  0x48480000

//
// The counter calls take the counter index in the low 16 bits of ECX, and
// return the count in EBX:EAX and the number of counters in ECX
//
#define SHV_CPUID_COUNT_MASK           0xFFFF
#define SHV_CPUID_COUNT_SELECTOR_MASK  0xFFFF0000

//
// The SimpleVisor signature, returned in EAX by the detection call and as
// the interface identifier of the Hyper-V interface leaf
//
#define SHV_CPUID_SIGNATURE            ' vhS'

//
// The unload confirmation, returned in ECX with the per-processor data
// pointer in EBX:EAX
//
#define SHV_CPUID_UNLOAD_RESULT        0x43434343
