# Euler

Euler is a hypervisor for 64-bit Windows and UEFI. It started as a fork of
[SimpleVisor](README.SimpleVisor.md), which Alex Ionescu wrote in 2016 to show
how little code it takes to virtualize a running system with VT-x. The history
of this repository goes back to his first commit. It also keeps the work of everyone
who contributed to SimpleVisor (Saar Amar, Joe Sylve, Satoshi Tanda, Petr Benes
and Zach Riggle). The files that came from SimpleVisor
keep their copyright notices, and its README is kept as it was.

SimpleVisor aimed to be as small as possible, and I wanted a bit more so Euler
is bigger. Euler also runs on AMD processors with SVM, and nested under Hyper-V,
OpenVMM and VMware. Under Hyper-V it forwards the guest's hypercalls, flushes
the guest's mappings when the guest flushes the TLB of other processors, and
idles when the guest halts. It maps all of physical memory with the memory types
the MTRRs give it.

The first commits maybe useful for SimpleVisor as they improve few things without
adding complexity.

Euler doubles the amount of Simple Visor code. There are about 6,800 lines of C and 500 of
assembly vs against SimpleVisor's 3,400 and 230. The newly added test harness (`vmlab`)
has about 3,000 lines of scripts that build Hyper-V, OpenVMM and VMware test VMs and
run Euler inside each.

## Layout

* `src` has the backend-independent code, with the VT-x and SVM backends in
  `src\vt-x` and `src\svm`, their assembly in `masm` under each, and the OS
  layers in `src\nt` and `src\uefi`.
* `inc` has the shared headers, with those of each backend in `inc\vt-x` and
  `inc\svm`.
* `vs` has the Visual Studio solution and project, `euler.slnx` and
  `euler.vcxproj`. Builds go to `x64\<configuration>`, as `euler.sys` for
  Windows and `euler.efi` for UEFI.
* `vmlab` has the test labs, see [vmlab\README.md](vmlab/README.md).

## Building

Euler needs:

* Visual Studio 2026 with the Desktop development with C++ workload, and the
  MSVC x64/x86 Spectre-mitigated libraries, which driver builds link with.
* The Windows Driver Kit with its Visual Studio extension, and the Windows SDK
  of the same version, 10.0.26100 or later. The build uses the newest one
  installed.

In Visual Studio, open `vs\euler.slnx`, pick the `NT` configuration and build.

From a Developer Command Prompt or Developer PowerShell for Visual Studio 2026,
in the root of the repo:

```
msbuild vs\euler.slnx /p:Configuration=NT
```

`NT` is also the default. The build writes `euler.sys`, `euler.pdb` and
`euler.cer` to `x64\NT`. It test-signs `euler.sys` with the WDK's test
certificate, `WDKTestCert <user>`, which it creates on first use, and
`euler.cer` is that certificate. Pass `/p:SignMode=Off` to leave the driver
unsigned.

Visual Studio 2022 builds Euler as well, with a WDK it has build tasks for. The
10.0.28000 WDK only has them for Visual Studio 2026, so if it is installed, pass
`/p:WindowsTargetPlatformVersion=10.0.26100.0` to use the 10.0.26100 one.

The labs build with the Enterprise WDK instead, which needs no Visual Studio,
see [vmlab\README.md](vmlab/README.md).

## Running

Loading the driver works as for SimpleVisor, see its
[README](README.SimpleVisor.md#installation-on-windows).
When unloading, Euler prints to the kernel debugger how many VM-exits it
handled per exit reason, how many TLB flush hypercalls it forwarded, and how
many IPIs it saw. `vmlab\guest\eulercheck.c` reads the same counters while
Euler runs.

Euler exits on external interrupts and injects them back, so it sees the
IPIs the guest sends through the local APIC's ICR. It counts those and its
own flush IPIs, which it sends to flush the guest's mappings on every
processor when it forwards the guest's TLB flush hypercalls and Hyper-V
offers no flush hypercall of its own. The flush IPIs never reach the guest.
Neither do NMIs: Euler never sends one itself, and the only NMI the guest
sees is the one `eulercheck nmi` asks Euler to inject on demand. An NMI
that reaches a running guest anyway halts Euler for the debugger, as
Windows crashes on NMIs.

Typical counts, from the Hyper-V lab: Windows Server 2025 on 4 processors,
nested under Hyper-V on Windows 11 24H2 on an Intel Core Ultra 9 185H, with
Euler loaded for about 20 seconds while a web browser runs. That's 81,121
VM-exits, about 1,000 per processor per second:

| VM-exit (VT-x exit reason) | Count | What Euler does |
|---|---:|---|
| CPUID (10) | 5,639 | Runs CPUID itself, and adjusts the result, such as with the Hypervisor Present-bit |
| HLT (12) | 5,073 | Idles in Hyper-V's guest idle MSR. Hyper-V makes HLT exits mandatory |
| VMCALL (18) | 15,908 | Passes the hypercall through to Hyper-V. 6,784 of them flushed the TLB (1,414 HvCallFlushVirtualAddressSpace and 5,370 HvCallFlushVirtualAddressList), after which Euler also flushed the guest's mappings on every processor |
| RDMSR (31) | 2,755 | Passes the read of a synthetic MSR through to Hyper-V |
| WRMSR (32) | 51,742 | Passes the write of a synthetic MSR through to Hyper-V |
| VM-entry failure (33) | 4 | Once per processor, as Hyper-V refuses the HLT activity state. HLT idles in Euler from then on |

## UEFI

The UEFI build isn't maintained, and the labs neither build nor test it. It
keeps SimpleVisor's limits: an OS loaded after it eventually crashes, as the
image runs in boot services memory, which the OS takes back after
ExitBootServices, and the processors other than the first call boot services,
which the PI specification doesn't allow. Besides, `src\uefi\simplevisor.inf`
builds an application, which EDK2 unloads as soon as its entry point returns,
and the UEFI configuration needs VisualUefi's EDK2, passed in `EDK_PATH`, as
with `msbuild vs\euler.slnx /p:Configuration=UEFI /p:EDK_PATH=<VisualUefi>\edk2`.
