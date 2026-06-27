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
  `euler.vcxproj`, which build with MSBuild, for example with
  `msbuild vs\euler.slnx /p:Configuration=NT`. Builds go
  to `x64\<configuration>`, as `euler.sys` for Windows and `euler.efi` for UEFI.
* `vmlab` has the test labs, see [vmlab\README.md](vmlab/README.md).

Loading the driver works as for SimpleVisor, see its
[README](README.SimpleVisor.md#installation-on-windows).
When unloading, the user mode utility prints number of VM exits and stats on some
hypercalls.

The UEFI build isn't maintained, and the labs neither build nor test it. It
keeps SimpleVisor's limits: an OS loaded after it eventually crashes, as the
image runs in boot services memory, which the OS takes back after
ExitBootServices, and the processors other than the first call boot services,
which the PI specification doesn't allow. Besides, `src\uefi\simplevisor.inf`
builds an application, which EDK2 unloads as soon as its entry point returns,
and the UEFI configuration needs VisualUefi's EDK2, passed in `EDK_PATH`.
