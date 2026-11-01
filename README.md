# Euler

`Euler` is a hypervisor for 64-bit Windows and UEFI. It started as a fork of
[SimpleVisor](README.SimpleVisor.md) which Alex Ionescu wrote in 2016 to show
how little code it takes to virtualize a running system with VT-x. The history
goes back to his first commit and keeps the work of everyone who contributed to
SimpleVisor (Saar Amar, Joe Sylve, Satoshi Tanda, Petr Benes and Zach Riggle).
Files from SimpleVisor keep their copyright notices and its README is kept as
it was.

SimpleVisor aimed to be as small as possible. I wanted a bit more so `Euler` is
bigger. It also runs on AMD with SVM and nested under Hyper-V, OpenVMM and
VMware. Under Hyper-V it forwards the guest's hypercalls, flushes the guest's
mappings when the guest flushes other processors' TLBs and idles when the
guest halts. It maps all of physical memory with the memory types from the
MTRRs.

The first commits may be useful for SimpleVisor as they improve a few things
without adding complexity.

`Euler` has more than twice SimpleVisor's code: about `8,500` lines of C and `650` of
assembly against `3,400` and `230`. The test harness, `vmlab`, adds about `3,300`
lines of scripts that build Hyper-V, OpenVMM and VMware VMs and run `Euler` in
each.

## Layout

* `src` has the backend-independent code. The VT-x and SVM backends are in
  `src\vt-x` and `src\svm` with their assembly in `masm` under each. The OS
  layers are in `src\nt` and `src\uefi`.
* `inc` has the shared headers and each backend's in `inc\vt-x` and
  `inc\svm`.
* `vs` has the solution and project, `euler.slnx` and `euler.vcxproj`. Builds
  go to `x64\<configuration>`: `euler.sys` for Windows, `euler.efi` for UEFI.
* `vmlab` has the test labs described in [vmlab\README.md](vmlab/README.md).

## Building

`Euler` needs:

* Visual Studio 2026 with the Desktop development with C++ workload and the
  Spectre-mitigated MSVC x64/x86 libraries which driver builds link with.
* The WDK with its Visual Studio extension and the Windows SDK of the same
  version, `10.0.26100` or later. The build uses the newest one installed.

In Visual Studio, open `vs\euler.slnx`, pick the `NT` configuration and build.
Or from a Developer Command Prompt or Developer PowerShell for Visual Studio
2026 in the root of the repo:

```
msbuild vs\euler.slnx /p:Configuration=NT
```

`NT` is the default. The build writes `euler.sys`, `euler.pdb` and `euler.cer`
to `x64\NT`. `euler.sys` is test-signed with the WDK's test certificate
(`WDKTestCert <user>`, created on first use) and `euler.cer` is that
certificate. Pass `/p:SignMode=Off` to skip signing.

Visual Studio 2022 works too with a WDK that has build tasks for it. The
`10.0.28000` WDK only has them for Visual Studio 2026 so pass
`/p:WindowsTargetPlatformVersion=10.0.26100.0` if it is installed.

The labs build with the Enterprise WDK which needs no Visual Studio.

## Running

Load the driver as described in SimpleVisor's
[README](README.SimpleVisor.md#installation-on-windows). On unload, `Euler`
prints to the kernel debugger how many VM-exits it handled per exit reason, how
many TLB flush hypercalls it forwarded and how many IPIs it saw: those the
guest sent through the x2APIC ICR and its own flush IPIs.

The guest flushes other processors' TLBs with hypercalls. `Euler` forwards them
then flushes the guest's mappings on every processor with Hyper-V's flush
hypercall if there is one and with flush IPIs otherwise. The flush IPIs are
NMIs which a processor takes in root mode even when the guest has interrupts
disabled. The sender waits for every processor to flush before the hypercall
returns. The IPIs go through the x2APIC ICR in x2APIC mode and through
Hyper-V's synthetic ICR in xAPIC mode where Hyper-V offers it as OpenVMM
does. If neither reaches a processor and Hyper-V has no flush hypercall, `Euler`
doesn't load as the guest would keep stale translations.

The flush IPIs never reach the guest and the guest takes its own interrupts
directly. Other NMIs are the guest's. Windows sends them to the other
processors when it bugchecks and when it freezes them for the kernel debugger
and one misses the freeze IPI which even a `DbgPrint` can cause with a
debugger enabled. `Euler` passes them on once the guest can take an NMI. On
VT-x the NMI window exit tells. SVM has none and injects an NMI even inside
the guest's NMI handler, so `Euler` tracks the guest's NMI blocking itself as
the guest only takes the NMIs `Euler` injects. It intercepts the IRET of the
guest's NMI handler and single-steps over it, and single-steps past the STI
and MOV SS shadow.

`Euler` emulates the address space switch hypercalls which Windows uses with
KVA shadow by switching the guest's `CR3`.

Typical counts from the Hyper-V and OpenVMM labs with Windows Server 2025 on
4 processors and `Euler` loaded for about 20 seconds while a web browser runs.
The host is Windows 11 24H2 on an Intel Core Ultra 9 185H. That's `132,440`
VM-exits under Hyper-V and `149,919` under OpenVMM, about `1,650` and `1,900` per
processor per second:

| VM-exit (VT-x exit reason) | Hyper-V | OpenVMM | What `Euler` does |
|---|---:|---:|---|
| Exception or NMI (`0`) | `0` | `37,141` | Flushes the guest's mappings on a flush IPI that arrives while the guest runs |
| CPUID (`10`) | `5,982` | `8,016` | Runs CPUID and adjusts the result, e.g. sets the Hypervisor Present-bit |
| HLT (`12`) | `5,914` | `15,842` | Idles in Hyper-V's guest idle MSR. Hyper-V makes HLT exits mandatory |
| VMCALL (`18`) | `16,414` | `17,478` | Forwards the hypercall to Hyper-V and flushes the guest's mappings on every processor after a TLB flush |
| RDMSR (`31`) | `3,371` | `15,369` | Forwards the read of a synthetic MSR to Hyper-V |
| WRMSR (`32`) | `100,755` | `56,069` | Forwards the write of a synthetic MSR or the x2APIC ICR to Hyper-V |
| VM-entry failure (`33`) | `4` | `4` | Once per processor as Hyper-V refuses the HLT activity state. HLT idles in `Euler` from then on |

Of the hypercalls, `7,862` flushed the TLB under Hyper-V (`7,821`
`HvCallFlushVirtualAddressSpace` and `41` `HvCallFlushVirtualAddressList`) and `13,495`
under OpenVMM (`193` and `13,302`). Hyper-V has a flush hypercall so `Euler` used
`HvCallFlushGuestPhysicalAddressSpace` and sent no flush IPIs. Its guest ran in
x2APIC mode and wrote the ICR `47,116` times and the self IPI register `6,261`
times. OpenVMM has no flush hypercall so `Euler` sent `53,705` flush IPIs through
the synthetic ICR counting the sending processor's own flushes. The processors
received `40,210` of them. Its guest ran in xAPIC mode whose ICR `Euler` doesn't
count.

The same labs on an AMD host, Windows 11 25H2 on an AMD Ryzen 7 3700X, count
`411,145` VM-exits under Hyper-V and `179,168` under OpenVMM, about `5,150` and
`2,250` per processor per second. `Euler` prints SVM exit code `0x72` plus `n`
as exit reason `n`, nested page faults as `28` and the other exit codes as
`79`:

| VM-exit (SVM exit code) | Hyper-V | OpenVMM | What `Euler` does |
|---|---:|---:|---|
| NMI (`0x61`) and others | `19,083` | `20,458` | Flushes the guest's mappings on a flush IPI that arrives while the guest runs |
| CPUID (`0x72`) | `4,255` | `4,286` | Runs CPUID and adjusts the result, e.g. sets the Hypervisor Present-bit |
| IRET (`0x74`) | `3` | `3` | Tells when the guest's NMI handler returns and it can take another NMI |
| HLT (`0x78`) | `21,212` | `10,345` | Idles in Hyper-V's guest idle MSR |
| MSR (`0x7C`) | `345,369` | `133,359` | Forwards the read or write of a synthetic MSR to Hyper-V as the MSR permission map doesn't cover them |
| VMMCALL (`0x81`) | `21,223` | `10,717` | Forwards the hypercall to Hyper-V and flushes the guest's mappings on every processor after a TLB flush |

Of the hypercalls, `6,898` flushed the TLB under Hyper-V (`189`
`HvCallFlushVirtualAddressSpace` and `6,709` `HvCallFlushVirtualAddressList`) and `6,901`
under OpenVMM (`244` and `6,657`). Neither Hyper-V on this host nor OpenVMM has
the flush hypercall so `Euler` sent `27,444` and `27,582` flush IPIs counting the
sending processor's own flushes. The processors received `20,546` and `20,681` of
them. Hyper-V hides x2APIC from its guest on this host so the guest ran in xAPIC
mode and the flush IPIs went through the synthetic ICR. The OpenVMM lab enables
the x2APIC from boot so they went through the x2APIC ICR there, and the guest
wrote neither the ICR nor the self IPI register. `Euler` passed `3` of the
guest's own NMIs on under each.

On bare metal, Windows 11 Insider build 26300 on an Intel Core i7-8550U with 8
logical processors and with the Windows hypervisor and VBS turned off, `Euler`
was loaded twice while every processor allocated and touched memory and
started processes for 90 seconds. That's `1,208,919` and `1,301,868` VM-exits.
Without Hyper-V there are no hypercalls to forward, no synthetic MSRs and no
HLT exits:

| VM-exit (VT-x exit reason) | First load | Second load | What `Euler` does |
|---|---:|---:|---|
| CPUID (`10`) | `301,044` | `313,879` | Runs CPUID and adjusts the result, e.g. sets the Hypervisor Present-bit |
| WRMSR (`32`) | `907,875` | `987,989` | Counts the write to the x2APIC ICR or the self IPI register and writes it |

Windows runs the local APIC in x2APIC mode there. It wrote the ICR `778,481`
and `836,282` times and the self IPI register `129,394` and `151,707` times.

## Testing and debugging

* `vmlab` builds `Euler`, loads it in a Hyper-V, OpenVMM or VMware VM, checks the
  guest and unloads it with a PASS or FAIL line per check. See
  [vmlab\README.md](vmlab/README.md).
* The guest probe, `vmlab\guest\eulercheck.c`, prints the CPUID state `Euler`
  affects. It reads the counters `Euler` prints on unload through a magic CPUID
  leaf. `eulercheck nmi` has `Euler` inject an NMI into the guest.
* On an exit it can't handle, `Euler` prints the exit and the guest state, saves
  them in the `Fatal*` fields of each processor's `SHV_VP_DATA` and breaks in.
* `euler!ShvExitTrace` holds the last `4,096` exits on all processors plus
  `Euler`'s own flush requests and NMIs. `vmlab\openvmm\Read-OvmmExitTrace.ps1`
  reads it and the fatal exits from the memory of a hung OpenVMM guest.
* `vmlab\Connect-LabDebugger.ps1` attaches `kd` or WinDbg to a lab VM. With
  `-AttachDebugger`, the lab tests keep `kd` attached and run
  `vmlab\kd-halt.txt` when the guest breaks in. It shows the bugcheck and each
  processor's guest stack at its fatal exit.

## UEFI

The UEFI build isn't maintained and the labs neither build nor test it. It
keeps SimpleVisor's limits. The image runs in boot services memory which the
OS takes back after `ExitBootServices` so an OS loaded after it eventually
crashes. The processors other than the first call boot services which the PI
specification doesn't allow. And `src\uefi\simplevisor.inf` builds an
application which EDK2 unloads as soon as its entry point returns.

The UEFI configuration needs VisualUefi's EDK2 in `EDK_PATH`:

```
msbuild vs\euler.slnx /p:Configuration=UEFI /p:EDK_PATH=<VisualUefi>\edk2
```
