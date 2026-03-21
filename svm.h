/*++

Copyright (c) Roman Kisel 2026.  All rights reserved.

Header Name:

    svm.h

Abstract:

    This header defines the MSRs and CPUID bits for AMD x64 SVM (AMD-V) support.

Author:

    kromych (vdso@mailbox.org) 13-Jun-2026 - Initial version

Environment:

    Kernel mode only.

--*/

#pragma once

//
// CPUID 0 vendor signatures, in EBX, EDX and ECX order
//
#define CPUID_VENDOR_AMD_EBX                    'htuA'  // "AuthenticAMD"
#define CPUID_VENDOR_AMD_EDX                    'itne'
#define CPUID_VENDOR_AMD_ECX                    'DMAc'
#define CPUID_VENDOR_HYGON_EBX                  'ogyH'  // "HygonGenuine"
#define CPUID_VENDOR_HYGON_EDX                  'neGn'
#define CPUID_VENDOR_HYGON_ECX                  'eniu'

//
// CPUID 8000_0001 ECX reports SVM. CPUID 8000_000A reports the SVM revision
// in EAX, the number of ASIDs in EBX and the SVM features in EDX
//
#define CPUID_EXTENDED_FEATURES                 0x80000001
#define CPUID_EXTENDED_FEATURES_ECX_SVM         0x00000004
#define CPUID_SVM_FEATURES                      0x8000000A
#define CPUID_SVM_FEATURES_EDX_NP               0x00000001
#define CPUID_SVM_FEATURES_EDX_NRIPS            0x00000008
#define CPUID_SVM_FEATURES_EDX_VMCB_CLEAN       0x00000020
#define CPUID_SVM_FEATURES_EDX_DECODE_ASSISTS   0x00000080

//
// EFER.SVME enables the SVM instructions
//
#define MSR_EFER                                0xC0000080
#define EFER_SVME                               (1ULL << 12)

//
// The firmware disables SVM with VM_CR.SVMDIS, and VM_CR.LOCK makes it read-only
//
#define MSR_VM_CR                               0xC0010114
#define VM_CR_LOCK                              (1ULL << 3)
#define VM_CR_SVMDIS                            (1ULL << 4)

//
// The physical address of the 4KB host save area that VMRUN saves the host
// state to, and #VMEXIT restores it from
//
#define MSR_VM_HSAVE_PA                         0xC0010117
