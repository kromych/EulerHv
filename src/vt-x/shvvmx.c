/*++

Copyright (c) Alex Ionescu.  All rights reserved.

Module Name:

    shvvmx.c

Abstract:

    This module implements Intel VMX (Vanderpool/VT-x)-specific routines.

Author:

    Alex Ionescu (@aionescu) 16-Mar-2016 - Initial version

Environment:

    Kernel mode only, IRQL DISPATCH_LEVEL.

--*/

#include "shvvmx.h"

VOID
ShvVmxEptInitialize (
    _In_ PSHV_VP_DATA VpData
    )
{
    UINT32 i, j;
    VMX_PDPTE tempEpdpte;
    VMX_LARGE_PDE tempEpde;
    VMX_HUGE_PDPTE tempHugePdpte;
    VMX_HUGE_PDPTE hugePdpte;
    VMX_PTE tempEpte;
    INT32 cpuInfo[4];
    UINT32 physicalAddressBits;
    UINT32 pml4Count;

    //
    // Fill out the EPML4E which covers the first 512GB of RAM
    //
    VpData->Epml4[0].Read = 1;
    VpData->Epml4[0].Write = 1;
    VpData->Epml4[0].Execute = 1;
    VpData->Epml4[0].PageFrameNumber = ShvOsGetPhysicalAddress(&VpData->Epdpt) / PAGE_SIZE;

    //
    // Fill out a RWX PDPTE
    //
    tempEpdpte.AsUlonglong = 0;
    tempEpdpte.Read = tempEpdpte.Write = tempEpdpte.Execute = 1;

    //
    // Construct EPT identity map for every 1GB of RAM
    //
    __stosq((UINT64*)VpData->Epdpt, tempEpdpte.AsUlonglong, PDPTE_ENTRY_COUNT);
    for (i = 0; i < PDPTE_ENTRY_COUNT; i++)
    {
        //
        // Set the page frame number of the PDE table
        //
        VpData->Epdpt[i].PageFrameNumber = ShvOsGetPhysicalAddress(&VpData->Epde[i][0]) / PAGE_SIZE;
    }

    //
    // Fill out a RWX Large PDE
    //
    tempEpde.AsUlonglong = 0;
    tempEpde.Read = tempEpde.Write = tempEpde.Execute = 1;
    tempEpde.Large = 1;

    //
    // Loop every 1GB of RAM (described by the PDPTE)
    //
    __stosq((UINT64*)VpData->Epde, tempEpde.AsUlonglong, PDPTE_ENTRY_COUNT * PDE_ENTRY_COUNT);
    for (i = 0; i < PDPTE_ENTRY_COUNT; i++)
    {
        //
        // Construct EPT identity map for every 2MB of RAM
        //
        for (j = 0; j < PDE_ENTRY_COUNT; j++)
        {
            VpData->Epde[i][j].PageFrameNumber = (i * 512) + j;
            VpData->Epde[i][j].Type = ShvUtilMtrrGetMemoryType(VpData,
                                                               (UINT64)VpData->Epde[i][j].PageFrameNumber * _2MB,
                                                               _2MB);
        }
    }

    //
    // The fixed-range MTRRs type the first 1MB in ranges as small as 4KB, such
    // as the UC legacy video memory, so map the first 2MB with 4KB pages. A
    // PDE that points to a page table has the layout of a PDPTE
    //
    tempEpte.AsUlonglong = 0;
    tempEpte.Read = tempEpte.Write = tempEpte.Execute = 1;
    for (i = 0; i < PTE_ENTRY_COUNT; i++)
    {
        VpData->EptLow[i].AsUlonglong = tempEpte.AsUlonglong;
        VpData->EptLow[i].PageFrameNumber = i;
        VpData->EptLow[i].Type = ShvUtilMtrrGetMemoryType(VpData,
                                                          (UINT64)i * PAGE_SIZE,
                                                          PAGE_SIZE);
    }
    tempEpdpte.PageFrameNumber = ShvOsGetPhysicalAddress(&VpData->EptLow) / PAGE_SIZE;
    VpData->Epde[0][0].AsUlonglong = tempEpdpte.AsUlonglong;

    //
    // The rest needs 1GB EPT pages. VMWare does not virtualize them correctly,
    // so under it keep the 2MB pages, and leave RAM or MMIO above 512GB
    // unmapped and let accesses halt
    //
    if (((VpData->MsrData[12].QuadPart & VMX_EPT_1GB_PAGE_BIT) == 0) ||
        (VpData->UnderlyingHypervisorIsVmware != FALSE))
    {
        return;
    }

    //
    // Fill out a RWX Huge PDPTE
    //
    tempHugePdpte.AsUlonglong = 0;
    tempHugePdpte.Read = tempHugePdpte.Write = tempHugePdpte.Execute = 1;
    tempHugePdpte.Large = 1;

    //
    // Where all of a 1GB has one memory type, map it with a 1GB page, which
    // shortens the EPT walk and keeps guest 1GB pages whole in the TLB. The
    // first 1GB has the 4KB pages of the first 2MB
    //
    for (i = 1; i < PDPTE_ENTRY_COUNT; i++)
    {
        for (j = 1; j < PDE_ENTRY_COUNT; j++)
        {
            if (VpData->Epde[i][j].Type != VpData->Epde[i][0].Type)
            {
                break;
            }
        }
        if (j == PDE_ENTRY_COUNT)
        {
            hugePdpte.AsUlonglong = tempHugePdpte.AsUlonglong;
            hugePdpte.PageFrameNumber = i;
            hugePdpte.Type = VpData->Epde[i][0].Type;
            VpData->Epdpt[i].AsUlonglong = hugePdpte.AsUlonglong;
        }
    }

    //
    // RAM or MMIO can live above 512GB. Map it too, at one page of PDPTEs per
    // 512GB. Each PML4 entry covers 512GB (39 bits) of the physical address
    // width
    //
    __cpuid(cpuInfo, 0x80000000);
    if ((UINT32)cpuInfo[0] < 0x80000008)
    {
        return;
    }
    __cpuid(cpuInfo, 0x80000008);
    physicalAddressBits = cpuInfo[0] & 0xFF;
    if ((physicalAddressBits <= 39) || (physicalAddressBits > 52))
    {
        return;
    }
    pml4Count = 1 << (physicalAddressBits - 39);
    if (pml4Count > SHV_EPT_PML4E_COUNT)
    {
        pml4Count = SHV_EPT_PML4E_COUNT;
    }

    //
    // Loop every additional 512GB, and identity map every 1GB of it
    //
    for (i = 1; i < pml4Count; i++)
    {
        VpData->Epml4[i].Read = 1;
        VpData->Epml4[i].Write = 1;
        VpData->Epml4[i].Execute = 1;
        VpData->Epml4[i].PageFrameNumber = ShvOsGetPhysicalAddress(&VpData->EpdptHuge[i - 1]) / PAGE_SIZE;

        for (j = 0; j < PDPTE_ENTRY_COUNT; j++)
        {
            VpData->EpdptHuge[i - 1][j].AsUlonglong = tempHugePdpte.AsUlonglong;
            VpData->EpdptHuge[i - 1][j].PageFrameNumber = (i * PDPTE_ENTRY_COUNT) + j;
            VpData->EpdptHuge[i - 1][j].Type = ShvUtilMtrrGetMemoryType(VpData,
                                                                        (UINT64)VpData->EpdptHuge[i - 1][j].PageFrameNumber * _1GB,
                                                                        _1GB);
        }
    }
}

UINT8
ShvVmxEnterRootModeOnVp (
    _In_ PSHV_VP_DATA VpData
    )
{
    PSHV_SPECIAL_REGISTERS Registers = &VpData->SpecialRegisters;

    //
    // Capture the revision ID for the VMXON and VMCS region
    //
    VpData->VmxOn.RevisionId = VpData->MsrData[0].LowPart;
    VpData->Vmcs.RevisionId = VpData->MsrData[0].LowPart;

    //
    // Store the physical addresses of all per-LP structures allocated
    //
    VpData->VmxOnPhysicalAddress = ShvOsGetPhysicalAddress(&VpData->VmxOn);
    VpData->VmcsPhysicalAddress = ShvOsGetPhysicalAddress(&VpData->Vmcs);
    VpData->MsrBitmapPhysicalAddress = ShvOsGetPhysicalAddress(VpData->MsrBitmap);
    VpData->EptPml4PhysicalAddress = ShvOsGetPhysicalAddress(&VpData->Epml4);

    //
    // Update CR0 with the must-be-zero and must-be-one requirements
    //
    Registers->Cr0 &= VpData->MsrData[7].LowPart;
    Registers->Cr0 |= VpData->MsrData[6].LowPart;

    //
    // Do the same for CR4, which sets VMXE. Remember whether it was clear, to
    // clear it again after VMXOFF
    //
    VpData->VmxSetVmxe = (Registers->Cr4 & CR4_VMXE) == 0;
    Registers->Cr4 &= VpData->MsrData[9].LowPart;
    Registers->Cr4 |= VpData->MsrData[8].LowPart;

    //
    // Update host CR0 and CR4 based on the requirements above
    //
    __writecr0(Registers->Cr0);
    __writecr4(Registers->Cr4);

    //
    // Enable VMX Root Mode
    //
    if (__vmx_on(&VpData->VmxOnPhysicalAddress))
    {
        if (VpData->VmxSetVmxe != FALSE)
        {
            __writecr4(__readcr4() & ~CR4_VMXE);
        }
        return FALSE;
    }

    //
    // Clear the state of the VMCS, setting it to Inactive
    //
    if (__vmx_vmclear(&VpData->VmcsPhysicalAddress))
    {
        ShvVmxOff(VpData);
        return FALSE;
    }

    //
    // Load the VMCS, setting its state to Active
    //
    if (__vmx_vmptrld(&VpData->VmcsPhysicalAddress))
    {
        ShvVmxOff(VpData);
        return FALSE;
    }

    //
    // VMX Root Mode is enabled, with an active VMCS.
    //
    return TRUE;
}

VOID
ShvVmxSetupVmcsForVp (
    _In_ PSHV_VP_DATA VpData
    )
{
    PSHV_SPECIAL_REGISTERS state = &VpData->SpecialRegisters;
    PCONTEXT context = &VpData->ContextFrame;
    VMX_GDTENTRY64 vmxGdtEntry;
    VMX_EPTP vmxEptp;
    UINT32 secondaryControls;

    //
    // Begin by setting the link pointer to the required value for 4KB VMCS.
    //
    __vmx_vmwrite(VMCS_LINK_POINTER, ~0ULL);

    //
    // Enable EPT features if supported
    //
    if (VpData->EptControls != 0)
    {
        //
        // Configure the EPTP
        //
        vmxEptp.AsUlonglong = 0;
        vmxEptp.PageWalkLength = 3;
        vmxEptp.Type = MTRR_TYPE_WB;
        vmxEptp.PageFrameNumber = VpData->EptPml4PhysicalAddress / PAGE_SIZE;

        //
        // Load EPT Root Pointer
        //
        __vmx_vmwrite(EPT_POINTER, vmxEptp.AsUlonglong);
        ShvVpSetSecondLevelRoot(VpData->ProcessorIndex, VpData->EptPml4PhysicalAddress);

        //
        // Set VPID to one
        //
        __vmx_vmwrite(VIRTUAL_PROCESSOR_ID, 1);
    }

    //
    // Load the MSR bitmap. Unlike other bitmaps, not having an MSR bitmap will
    // trap all MSRs, so we allocated an empty one.
    //
    __vmx_vmwrite(MSR_BITMAP, VpData->MsrBitmapPhysicalAddress);

    //
    // Enable support for RDTSCP and XSAVES/XRESTORES in the guest. Windows 10
    // makes use of both of these instructions if the CPU supports it. By using
    // ShvUtilAdjustMsr, these options will be ignored if this processor does
    // not actually support the instructions to begin with.
    //
    // Also enable EPT support, for additional performance and ability to trap
    // memory access efficiently.
    //
    // Finally, enable UMONITOR, UMWAIT and TPAUSE, which otherwise raise #UD
    // although CPUID reports WAITPKG. Without the control the CPUID handler
    // hides WAITPKG
    //
    secondaryControls = ShvUtilAdjustMsr(VpData->MsrData[11],
                                         SECONDARY_EXEC_ENABLE_RDTSCP |
                                         SECONDARY_EXEC_ENABLE_INVPCID |
                                         SECONDARY_EXEC_XSAVES |
                                         SECONDARY_EXEC_ENABLE_USR_WAIT_PAUSE |
                                         VpData->EptControls);
    VpData->UserWaitPauseEnabled =
        (secondaryControls & SECONDARY_EXEC_ENABLE_USR_WAIT_PAUSE) != 0;
    __vmx_vmwrite(SECONDARY_VM_EXEC_CONTROL, secondaryControls);

    //
    // Enable no pin-based options ourselves, but there may be some required by
    // the processor. Use ShvUtilAdjustMsr to add those in.
    //
    __vmx_vmwrite(PIN_BASED_VM_EXEC_CONTROL,
                           ShvUtilAdjustMsr(VpData->MsrData[13], 0));

    //
    // In order for our choice of supporting RDTSCP and XSAVE/RESTORES above to
    // actually mean something, we have to request secondary controls. We also
    // want to activate the MSR bitmap in order to keep them from being caught.
    //
    __vmx_vmwrite(CPU_BASED_VM_EXEC_CONTROL,
                           ShvUtilAdjustMsr(VpData->MsrData[14],
                                            CPU_BASED_ACTIVATE_MSR_BITMAP |
                                            CPU_BASED_ACTIVATE_SECONDARY_CONTROLS));

    //
    // Make sure to enter us in x64 mode at all times. VM-Exits set DR7 to 400h
    // and clear IA32_DEBUGCTL, so save the guest ones, which the true controls
    // let processors leave out
    //
    __vmx_vmwrite(VM_EXIT_CONTROLS,
                           ShvUtilAdjustMsr(VpData->MsrData[15],
                                            VM_EXIT_IA32E_MODE |
                                            VM_EXIT_SAVE_DEBUG_CNTRLS));

    //
    // As we exit back into the guest, make sure to exist in x64 mode as well,
    // with the guest DR7 and IA32_DEBUGCTL loaded back
    //
    __vmx_vmwrite(VM_ENTRY_CONTROLS,
                           ShvUtilAdjustMsr(VpData->MsrData[16],
                                            VM_ENTRY_IA32E_MODE |
                                            VM_ENTRY_LOAD_DEBUG_CNTRLS));

    //
    // Load the CS Segment (Ring 0 Code)
    //
    ShvUtilConvertGdtEntry(state->Gdtr.Base, context->SegCs, &vmxGdtEntry);
    __vmx_vmwrite(GUEST_CS_SELECTOR, vmxGdtEntry.Selector);
    __vmx_vmwrite(GUEST_CS_LIMIT, vmxGdtEntry.Limit);
    __vmx_vmwrite(GUEST_CS_AR_BYTES, vmxGdtEntry.AccessRights);
    __vmx_vmwrite(GUEST_CS_BASE, vmxGdtEntry.Base);
    __vmx_vmwrite(HOST_CS_SELECTOR, context->SegCs & ~RPL_MASK);

    //
    // Load the SS Segment (Ring 0 Data)
    //
    ShvUtilConvertGdtEntry(state->Gdtr.Base, context->SegSs, &vmxGdtEntry);
    __vmx_vmwrite(GUEST_SS_SELECTOR, vmxGdtEntry.Selector);
    __vmx_vmwrite(GUEST_SS_LIMIT, vmxGdtEntry.Limit);
    __vmx_vmwrite(GUEST_SS_AR_BYTES, vmxGdtEntry.AccessRights);
    __vmx_vmwrite(GUEST_SS_BASE, vmxGdtEntry.Base);
    __vmx_vmwrite(HOST_SS_SELECTOR, context->SegSs & ~RPL_MASK);

    //
    // Load the DS Segment (Ring 3 Data)
    //
    ShvUtilConvertGdtEntry(state->Gdtr.Base, context->SegDs, &vmxGdtEntry);
    __vmx_vmwrite(GUEST_DS_SELECTOR, vmxGdtEntry.Selector);
    __vmx_vmwrite(GUEST_DS_LIMIT, vmxGdtEntry.Limit);
    __vmx_vmwrite(GUEST_DS_AR_BYTES, vmxGdtEntry.AccessRights);
    __vmx_vmwrite(GUEST_DS_BASE, vmxGdtEntry.Base);
    __vmx_vmwrite(HOST_DS_SELECTOR, context->SegDs & ~RPL_MASK);

    //
    // Load the ES Segment (Ring 3 Data)
    //
    ShvUtilConvertGdtEntry(state->Gdtr.Base, context->SegEs, &vmxGdtEntry);
    __vmx_vmwrite(GUEST_ES_SELECTOR, vmxGdtEntry.Selector);
    __vmx_vmwrite(GUEST_ES_LIMIT, vmxGdtEntry.Limit);
    __vmx_vmwrite(GUEST_ES_AR_BYTES, vmxGdtEntry.AccessRights);
    __vmx_vmwrite(GUEST_ES_BASE, vmxGdtEntry.Base);
    __vmx_vmwrite(HOST_ES_SELECTOR, context->SegEs & ~RPL_MASK);

    //
    // Load the FS Segment (Ring 3 Compatibility-Mode TEB)
    //
    ShvUtilConvertGdtEntry(state->Gdtr.Base, context->SegFs, &vmxGdtEntry);
    __vmx_vmwrite(GUEST_FS_SELECTOR, vmxGdtEntry.Selector);
    __vmx_vmwrite(GUEST_FS_LIMIT, vmxGdtEntry.Limit);
    __vmx_vmwrite(GUEST_FS_AR_BYTES, vmxGdtEntry.AccessRights);
    __vmx_vmwrite(GUEST_FS_BASE, vmxGdtEntry.Base);
    __vmx_vmwrite(HOST_FS_BASE, vmxGdtEntry.Base);
    __vmx_vmwrite(HOST_FS_SELECTOR, context->SegFs & ~RPL_MASK);

    //
    // Load the GS Segment (Ring 3 Data if in Compatibility-Mode, MSR-based in Long Mode)
    //
    ShvUtilConvertGdtEntry(state->Gdtr.Base, context->SegGs, &vmxGdtEntry);
    __vmx_vmwrite(GUEST_GS_SELECTOR, vmxGdtEntry.Selector);
    __vmx_vmwrite(GUEST_GS_LIMIT, vmxGdtEntry.Limit);
    __vmx_vmwrite(GUEST_GS_AR_BYTES, vmxGdtEntry.AccessRights);
    __vmx_vmwrite(GUEST_GS_BASE, state->MsrGsBase);
    __vmx_vmwrite(HOST_GS_BASE, state->MsrGsBase);
    __vmx_vmwrite(HOST_GS_SELECTOR, context->SegGs & ~RPL_MASK);

    //
    // Load the Task Register (Ring 0 TSS)
    //
    ShvUtilConvertGdtEntry(state->Gdtr.Base, state->Tr, &vmxGdtEntry);
    __vmx_vmwrite(GUEST_TR_SELECTOR, vmxGdtEntry.Selector);
    __vmx_vmwrite(GUEST_TR_LIMIT, vmxGdtEntry.Limit);
    __vmx_vmwrite(GUEST_TR_AR_BYTES, vmxGdtEntry.AccessRights);
    __vmx_vmwrite(GUEST_TR_BASE, vmxGdtEntry.Base);
    __vmx_vmwrite(HOST_TR_BASE, vmxGdtEntry.Base);
    __vmx_vmwrite(HOST_TR_SELECTOR, state->Tr & ~RPL_MASK);

    //
    // Load the Local Descriptor Table (Ring 0 LDT on Redstone)
    //
    ShvUtilConvertGdtEntry(state->Gdtr.Base, state->Ldtr, &vmxGdtEntry);
    __vmx_vmwrite(GUEST_LDTR_SELECTOR, vmxGdtEntry.Selector);
    __vmx_vmwrite(GUEST_LDTR_LIMIT, vmxGdtEntry.Limit);
    __vmx_vmwrite(GUEST_LDTR_AR_BYTES, vmxGdtEntry.AccessRights);
    __vmx_vmwrite(GUEST_LDTR_BASE, vmxGdtEntry.Base);

    //
    // Now load the GDT itself
    //
    __vmx_vmwrite(GUEST_GDTR_BASE, (uintptr_t)state->Gdtr.Base);
    __vmx_vmwrite(GUEST_GDTR_LIMIT, state->Gdtr.Limit);
    __vmx_vmwrite(HOST_GDTR_BASE, (uintptr_t)state->Gdtr.Base);

    //
    // And then the IDT
    //
    __vmx_vmwrite(GUEST_IDTR_BASE, (uintptr_t)state->Idtr.Base);
    __vmx_vmwrite(GUEST_IDTR_LIMIT, state->Idtr.Limit);
    __vmx_vmwrite(HOST_IDTR_BASE, (uintptr_t)state->Idtr.Base);
    ShvUtilBuildIdleIdt(VpData);

    //
    // Load CR0
    //
    __vmx_vmwrite(CR0_READ_SHADOW, state->Cr0);
    __vmx_vmwrite(HOST_CR0, state->Cr0);
    __vmx_vmwrite(GUEST_CR0, state->Cr0);

    //
    // Load CR3 -- do not use the current process' address space for the host,
    // because we may be executing in an arbitrary user-mode process right now
    // as part of the DPC interrupt we execute in.
    //
    __vmx_vmwrite(HOST_CR3, VpData->SystemDirectoryTableBase);
    __vmx_vmwrite(GUEST_CR3, state->Cr3);

    //
    // Load CR4
    //
    __vmx_vmwrite(HOST_CR4, state->Cr4);
    __vmx_vmwrite(GUEST_CR4, state->Cr4);
    __vmx_vmwrite(CR4_READ_SHADOW, state->Cr4);

    //
    // Load debug MSR and register (DR7)
    //
    __vmx_vmwrite(GUEST_IA32_DEBUGCTL, state->DebugControl);
    __vmx_vmwrite(GUEST_DR7, state->KernelDr7);

    //
    // Finally, load the guest stack, instruction pointer, and rflags, which
    // corresponds exactly to the location where RtlCaptureContext will return
    // to inside of ShvVpInitialize.
    //
    // ShvVpRestoreAfterLaunch briefly runs on the hypervisor stack, so enter
    // it with interrupts off. An exiting instruction in an ISR there would
    // have its frames overwritten. ShvOsRestoreContext restores the real
    // EFLAGS.
    //
    // The 8 byte bias stands in for a return address, which keeps RSP
    // ABI-aligned on entry.
    //
    __vmx_vmwrite(GUEST_RSP, (uintptr_t)VpData->ShvStackLimit + KERNEL_STACK_SIZE - sizeof(CONTEXT) - sizeof(UINT64));
    __vmx_vmwrite(GUEST_RIP, (uintptr_t)ShvVpRestoreAfterLaunch);
    __vmx_vmwrite(GUEST_RFLAGS, context->EFlags & ~EFLAGS_INTERRUPT_FLAG);

    //
    // Load the hypervisor entrypoint and stack. We give ourselves a standard
    // size kernel stack (24KB) and bias for the context structure that the
    // hypervisor entrypoint will push on the stack, avoiding the need for RSP
    // modifying instructions in the entrypoint. Note that the CONTEXT pointer
    // and thus the stack itself, must be 16-byte aligned for ABI compatibility
    // with AMD64 -- specifically, XMM operations will fail otherwise, such as
    // the ones that RtlCaptureContext will perform.
    //
    C_ASSERT((KERNEL_STACK_SIZE - sizeof(CONTEXT)) % 16 == 0);
    __vmx_vmwrite(HOST_RSP, (uintptr_t)VpData->ShvStackLimit + KERNEL_STACK_SIZE - sizeof(CONTEXT));
    __vmx_vmwrite(HOST_RIP, (uintptr_t)ShvVmxEntry);
}

UINT8
ShvVmxProbe (
    _In_ PSHV_VP_DATA VpData
    )
{
    INT32 cpu_info[4];
    UINT64 featureControl;
    UINT32 i;

    //
    // Check the VMX feature bit
    //
    __cpuid(cpu_info, 1);
    if ((cpu_info[2] & 0x20) == FALSE)
    {
        return FALSE;
    }

    //
    // Check if the Feature Control MSR is locked. If it isn't, this means that
    // BIOS/UEFI firmware screwed up, and we could go around locking it, but
    // we'd rather not mess with it.
    //
    featureControl = __readmsr(IA32_FEATURE_CONTROL_MSR);
    if (!(featureControl & IA32_FEATURE_CONTROL_MSR_LOCK))
    {
        return FALSE;
    }

    //
    // The Feature Control MSR is locked-in (valid). Is VMX enabled in normal
    // operation mode?
    //
    if (!(featureControl & IA32_FEATURE_CONTROL_MSR_ENABLE_VMXON_OUTSIDE_SMX))
    {
        return FALSE;
    }

    //
    // Both the hardware and the firmware are allowing us to enter VMX mode.
    // Initialize the VMX-related MSRs by reading their value, up to the VMCS
    // enumeration, which every processor has
    //
    for (i = 0; i <= MSR_IA32_VMX_VMCS_ENUM - MSR_IA32_VMX_BASIC; i++)
    {
        VpData->MsrData[i].QuadPart = __readmsr(MSR_IA32_VMX_BASIC + i);
    }

    //
    // Ensure the the VMCS can fit into a single page
    //
    if (((VpData->MsrData[0].QuadPart & VMX_BASIC_VMCS_SIZE_MASK) >> 32) > PAGE_SIZE)
    {
        return FALSE;
    }

    //
    // Ensure that the VMCS is supported in writeback memory
    //
    if (((VpData->MsrData[0].QuadPart & VMX_BASIC_MEMORY_TYPE_MASK) >> 50) != MTRR_TYPE_WB)
    {
        return FALSE;
    }

    //
    // Ensure that true MSRs can be used for capabilities
    //
    if (((VpData->MsrData[0].QuadPart) & VMX_BASIC_DEFAULT1_ZERO) == 0)
    {
        return FALSE;
    }

    //
    // The others exist only with the features they describe. The secondary
    // controls need the primary control that activates them, and the EPT and
    // VPID capabilities need EPT or VPID. The true controls exist, as checked
    // above. Missing ones stay zero, which allows no controls
    //
    if ((VpData->MsrData[MSR_IA32_VMX_PROCBASED_CTLS - MSR_IA32_VMX_BASIC].HighPart &
         CPU_BASED_ACTIVATE_SECONDARY_CONTROLS) != 0)
    {
        VpData->MsrData[MSR_IA32_VMX_PROCBASED_CTLS2 - MSR_IA32_VMX_BASIC].QuadPart =
            __readmsr(MSR_IA32_VMX_PROCBASED_CTLS2);

        if ((VpData->MsrData[MSR_IA32_VMX_PROCBASED_CTLS2 - MSR_IA32_VMX_BASIC].HighPart &
             (SECONDARY_EXEC_ENABLE_EPT | SECONDARY_EXEC_ENABLE_VPID)) != 0)
        {
            VpData->MsrData[MSR_IA32_VMX_EPT_VPID_CAP - MSR_IA32_VMX_BASIC].QuadPart =
                __readmsr(MSR_IA32_VMX_EPT_VPID_CAP);
        }
    }
    for (i = MSR_IA32_VMX_TRUE_PINBASED_CTLS - MSR_IA32_VMX_BASIC;
         i < sizeof(VpData->MsrData) / sizeof(VpData->MsrData[0]);
         i++)
    {
        VpData->MsrData[i].QuadPart = __readmsr(MSR_IA32_VMX_BASIC + i);
    }

    //
    // Ensure that EPT is available with the needed features SimpleVisor uses
    //
    if (((VpData->MsrData[12].QuadPart & VMX_EPT_PAGE_WALK_4_BIT) != 0) &&
        ((VpData->MsrData[12].QuadPart & VMX_EPTP_WB_BIT) != 0) &&
        ((VpData->MsrData[12].QuadPart & VMX_EPT_2MB_PAGE_BIT) != 0))
    {
        //
        // Enable EPT if these features are supported
        //
        VpData->EptControls = SECONDARY_EXEC_ENABLE_EPT | SECONDARY_EXEC_ENABLE_VPID;
    }
    return TRUE;
}

//
// The VT-x backend. ShvVmxLaunch launches the VMCS, and VM-Exits enter at
// ShvVmxEntry, which is the HOST_RIP that ShvVmxSetupVmcsForVp sets
//
const SHV_ARCH ShvVmxArch =
{
    ShvVmxProbe,
    ShvVmxEptInitialize,
    ShvVmxEnterRootModeOnVp,
    ShvVmxSetupVmcsForVp,
    ShvVmxLaunch,
    ShvVmxHandleExit,
};

