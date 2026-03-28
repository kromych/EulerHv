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
#define CPUID_EXTENDED_FEATURES_EDX_PAGE_1GB    0x04000000
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

//
// The nested page tables use the AMD64 long mode page table format. Nested
// walks count as user accesses, so every level needs User set. There are no
// memory-type fields: PWT, PCD and PAT index the host PAT, and the result is
// combined with the guest PAT type and the host MTRRs
//
typedef struct _SVM_NPT_PML4E
{
    union
    {
        struct
        {
            UINT64 Present : 1;
            UINT64 Write : 1;
            UINT64 User : 1;
            UINT64 WriteThrough : 1;
            UINT64 CacheDisable : 1;
            UINT64 Accessed : 1;
            UINT64 Ignored : 1;
            UINT64 Reserved : 2;
            UINT64 SoftwareUse : 3;
            UINT64 PageFrameNumber : 40;
            UINT64 SoftwareUseHigh : 11;
            UINT64 NoExecute : 1;
        };
        UINT64 AsUlonglong;
    };
} SVM_NPT_PML4E, *PSVM_NPT_PML4E;

typedef struct _SVM_NPT_PDPTE
{
    union
    {
        struct
        {
            UINT64 Present : 1;
            UINT64 Write : 1;
            UINT64 User : 1;
            UINT64 WriteThrough : 1;
            UINT64 CacheDisable : 1;
            UINT64 Accessed : 1;
            UINT64 Ignored : 1;
            UINT64 Large : 1;
            UINT64 Ignored2 : 1;
            UINT64 SoftwareUse : 3;
            UINT64 PageFrameNumber : 40;
            UINT64 SoftwareUseHigh : 11;
            UINT64 NoExecute : 1;
        };
        UINT64 AsUlonglong;
    };
} SVM_NPT_PDPTE, *PSVM_NPT_PDPTE;

typedef struct _SVM_NPT_HUGE_PDPTE
{
    union
    {
        struct
        {
            UINT64 Present : 1;
            UINT64 Write : 1;
            UINT64 User : 1;
            UINT64 WriteThrough : 1;
            UINT64 CacheDisable : 1;
            UINT64 Accessed : 1;
            UINT64 Dirty : 1;
            UINT64 Large : 1;
            UINT64 Global : 1;
            UINT64 SoftwareUse : 3;
            UINT64 Pat : 1;
            UINT64 Reserved : 17;
            UINT64 PageFrameNumber : 22;
            UINT64 SoftwareUseHigh : 11;
            UINT64 NoExecute : 1;
        };
        UINT64 AsUlonglong;
    };
} SVM_NPT_HUGE_PDPTE, *PSVM_NPT_HUGE_PDPTE;

typedef struct _SVM_NPT_LARGE_PDE
{
    union
    {
        struct
        {
            UINT64 Present : 1;
            UINT64 Write : 1;
            UINT64 User : 1;
            UINT64 WriteThrough : 1;
            UINT64 CacheDisable : 1;
            UINT64 Accessed : 1;
            UINT64 Dirty : 1;
            UINT64 Large : 1;
            UINT64 Global : 1;
            UINT64 SoftwareUse : 3;
            UINT64 Pat : 1;
            UINT64 Reserved : 8;
            UINT64 PageFrameNumber : 31;
            UINT64 SoftwareUseHigh : 11;
            UINT64 NoExecute : 1;
        };
        UINT64 AsUlonglong;
    };
} SVM_NPT_LARGE_PDE, *PSVM_NPT_LARGE_PDE;

static_assert(sizeof(SVM_NPT_PML4E) == sizeof(UINT64), "NPML4E Size Mismatch");
static_assert(sizeof(SVM_NPT_PDPTE) == sizeof(UINT64), "NPDPTE Size Mismatch");
static_assert(sizeof(SVM_NPT_HUGE_PDPTE) == sizeof(UINT64), "Huge NPDPTE Size Mismatch");
static_assert(sizeof(SVM_NPT_LARGE_PDE) == sizeof(UINT64), "NPDE Size Mismatch");

//
// Intercept word 1 (VMCB offset 0x00C) and word 2 (VMCB offset 0x010)
//
#define SVM_INTERCEPT_CPUID                     (1UL << 18)
#define SVM_INTERCEPT_INVD                      (1UL << 22)
#define SVM_INTERCEPT_INVLPGA                   (1UL << 26)
#define SVM_INTERCEPT_MSR_PROT                  (1UL << 28)

#define SVM_INTERCEPT_VMRUN                     (1UL << 0)
#define SVM_INTERCEPT_VMMCALL                   (1UL << 1)
#define SVM_INTERCEPT_VMLOAD                    (1UL << 2)
#define SVM_INTERCEPT_VMSAVE                    (1UL << 3)
#define SVM_INTERCEPT_STGI                      (1UL << 4)
#define SVM_INTERCEPT_CLGI                      (1UL << 5)
#define SVM_INTERCEPT_SKINIT                    (1UL << 6)
#define SVM_INTERCEPT_XSETBV                    (1UL << 13)

//
// TLB_CONTROL (VMCB offset 0x05C). Value 1 flushes the entire TLB on VMRUN,
// which is needed for the first entry with a new ASID
//
#define SVM_TLB_CONTROL_FLUSH_ALL               1

//
// VMCB clean bits (VMCB offset 0x0C0). A clear bit makes VMRUN reload the
// fields of its group
//
#define SVM_VMCB_CLEAN_I                        (1UL << 0)
#define SVM_VMCB_CLEAN_IOPM                     (1UL << 1)
#define SVM_VMCB_CLEAN_ASID                     (1UL << 2)
#define SVM_VMCB_CLEAN_TPR                      (1UL << 3)
#define SVM_VMCB_CLEAN_NP                       (1UL << 4)
#define SVM_VMCB_CLEAN_CRX                      (1UL << 5)
#define SVM_VMCB_CLEAN_DRX                      (1UL << 6)
#define SVM_VMCB_CLEAN_DT                       (1UL << 7)
#define SVM_VMCB_CLEAN_SEG                      (1UL << 8)
#define SVM_VMCB_CLEAN_CR2                      (1UL << 9)
#define SVM_VMCB_CLEAN_LBR                      (1UL << 10)
#define SVM_VMCB_CLEAN_AVIC                     (1UL << 11)

//
// SVM exit codes (VMCB offset 0x070). The codes match the intercept bit
// positions, starting at 0x60 for the miscellaneous intercepts
//
#define VMEXIT_CPUID            0x72
#define VMEXIT_INVD             0x76
#define VMEXIT_HLT              0x78
#define VMEXIT_INVLPGA          0x7A
#define VMEXIT_MSR              0x7C
#define VMEXIT_VMRUN            0x80
#define VMEXIT_VMMCALL          0x81
#define VMEXIT_VMLOAD           0x82
#define VMEXIT_VMSAVE           0x83
#define VMEXIT_STGI             0x84
#define VMEXIT_CLGI             0x85
#define VMEXIT_SKINIT           0x86
#define VMEXIT_XSETBV           0x8D
#define VMEXIT_NPF              0x400
#define VMEXIT_INVALID          ((UINT64)-1)

//
// VMCB EVENTINJ field format (offset 0x0A8). Type 3 is a hardware exception.
// The error code occupies the upper 32 bits when EV is set
//
#define SVM_EVENTINJ_TYPE_EXCEPTION     (3 << 8)
#define SVM_EVENTINJ_DELIVER_ERROR_CODE (1 << 11)
#define SVM_EVENTINJ_VALID              (1UL << 31)

//
// VMCB segment register format. Unlike the VMX access rights, the attributes
// have AVL, L, D/B and G at bits 8-11
//
typedef struct _SVM_VMCB_SEGMENT
{
    UINT16 Selector;
    UINT16 Attrib;
    UINT32 Limit;
    UINT64 Base;
} SVM_VMCB_SEGMENT, *PSVM_VMCB_SEGMENT;

static_assert(sizeof(SVM_VMCB_SEGMENT) == 16, "VMCB Segment Size Mismatch");

//
// The VMCB is a 4KB page split into a control area (0x000-0x3FF) and a state
// save area (0x400-0xFFF). Only the fields SimpleVisor uses are named
//
typedef struct _SVM_VMCB
{
    union
    {
        struct
        {
            UINT8 ControlArea0[0x0C];           // 0x000: CR/DR intercepts,
                                                //        exception bitmap
            UINT32 InterceptMisc1;              // 0x00C
            UINT32 InterceptMisc2;              // 0x010
            UINT8 ControlArea1[0x34];           // 0x014
            UINT64 MsrpmBasePa;                 // 0x048
            UINT8 ControlArea2[0x08];           // 0x050: TSC offset
            UINT32 GuestAsid;                   // 0x058
            UINT32 TlbControl;                  // 0x05C
            UINT8 ControlArea3[0x10];           // 0x060: virtual interrupt
            UINT64 ExitCode;                    // 0x070
            UINT64 ExitInfo1;                   // 0x078
            UINT64 ExitInfo2;                   // 0x080
            UINT64 ExitIntInfo;                 // 0x088
            UINT64 NpEnable;                    // 0x090
            UINT8 ControlArea4[0x10];           // 0x098: AVIC, GHCB
            UINT64 EventInj;                    // 0x0A8
            UINT64 NCr3;                        // 0x0B0
            UINT8 ControlArea5[0x08];           // 0x0B8: LBR
            UINT32 VmcbCleanBits;               // 0x0C0
            UINT32 ControlArea6;                // 0x0C4
            UINT64 NextRip;                     // 0x0C8
            UINT8 ControlArea7[0x330];          // 0x0D0-0x3FF

            SVM_VMCB_SEGMENT Es;                // 0x400
            SVM_VMCB_SEGMENT Cs;                // 0x410
            SVM_VMCB_SEGMENT Ss;                // 0x420
            SVM_VMCB_SEGMENT Ds;                // 0x430
            SVM_VMCB_SEGMENT Fs;                // 0x440
            SVM_VMCB_SEGMENT Gs;                // 0x450
            SVM_VMCB_SEGMENT Gdtr;              // 0x460
            SVM_VMCB_SEGMENT Ldtr;              // 0x470
            SVM_VMCB_SEGMENT Idtr;              // 0x480
            SVM_VMCB_SEGMENT Tr;                // 0x490
            UINT8 StateArea0[0x2B];             // 0x4A0
            UINT8 Cpl;                          // 0x4CB
            UINT8 StateArea1_[0x04];            // 0x4CC
            UINT64 Efer;                        // 0x4D0
            UINT8 StateArea2[0x70];             // 0x4D8
            UINT64 Cr4;                         // 0x548
            UINT64 Cr3;                         // 0x550
            UINT64 Cr0;                         // 0x558
            UINT64 Dr7;                         // 0x560
            UINT64 Dr6;                         // 0x568
            UINT64 Rflags;                      // 0x570
            UINT64 Rip;                         // 0x578
            UINT8 StateArea3[0x58];             // 0x580
            UINT64 Rsp;                         // 0x5D8
            UINT8 StateArea4[0x18];             // 0x5E0
            UINT64 Rax;                         // 0x5F8
            UINT8 StateArea5[0x400];            // 0x600-0x9FF
            UINT8 AvailableForSoftware[0x600];  // 0xA00-0xFFF
        };
        UINT8 Raw[PAGE_SIZE];
    };
} SVM_VMCB, *PSVM_VMCB;

static_assert(sizeof(SVM_VMCB) == PAGE_SIZE, "VMCB Size Mismatch");
