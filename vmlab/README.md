# SimpleVisor lab

These scripts build a VM for loading SimpleVisor, unloading it, and checking
what the guest sees while it's loaded. When SimpleVisor halts or the guest
bugchecks, you can debug it. There are three flavors, which share the build, the
guest setup, the guest test and the checks:

- **VMware Workstation** (this directory). The guest sees VMware underneath, so
  the lab exercises SimpleVisor's paths for VMware: its detection, passing the
  hypervisor CPUID leaves through, and the EPT limits. VMware needs a host
  without Hyper-V or VBS to nest VT-x.
- **OpenVMM** (`openvmm\`). OpenVMM runs on the Windows Hypervisor Platform and
  can nest VT-x even on hosts that run Hyper-V or VBS. The guest sees Hyper-V
  underneath (`Hv#1`), so the lab exercises SimpleVisor's paths for Hyper-V. See
  [The OpenVMM lab](#the-openvmm-lab).
- **Hyper-V** (`hyperv\`). Hyper-V nests VT-x with its nested virtualization,
  so SimpleVisor runs under the real Hyper-V, with VMBus. It needs no builds
  besides SimpleVisor's. See [The Hyper-V lab](#the-hyper-v-lab).

## Requirements

- **VMware Workstation Pro 17** or newer (free for personal and commercial use).
- **No hypervisor on the host.** VMware can only give a guest VT-x ("Virtualize
  Intel VT-x/EPT") when it owns VT-x itself. If Hyper-V runs on the host, VMware
  runs on top of it and can't nest, and SimpleVisor has no VT-x to load on. VBS
  features count too, such as Memory Integrity and Credential Guard. The scripts
  check this and say what to turn off.
- **An Intel or AMD CPU.** SimpleVisor uses VT-x on Intel and SVM on AMD, and
  the guest test accepts either. The labs have only been run on Intel so far.
- **The Enterprise WDK**, used to build SimpleVisor, sign it and run the
  debugger. By default the scripts look for it in a `wdk` directory next to this
  repo. Set `SHV_WDK_ROOT` or pass `-WdkRoot` to use another location.
- **The Windows ADK "Deployment Tools"** feature, for `oscdimg.exe`.
- **A 64-bit Windows 10, Windows 11 or Windows Server ISO.** Evaluation media
  is easiest, because it needs no product key. The
  [Windows Server 2025 evaluation](https://www.microsoft.com/en-us/evalcenter/download-windows-server-2025)
  ISO works well.
- **PowerShell**, Windows PowerShell 5.1 or PowerShell 7. You don't need an
  elevated prompt.

## Usage

```powershell
cd vmlab

# Once: create the VM. This takes about 20-40 minutes and is fully unattended.
.\New-LabVm.ps1 -IsoPath D:\iso\Server2025_Eval.iso -ImageName 'Windows Server 2025 SERVERSTANDARD'

# Every time: build, deploy, load, check, unload.
.\Invoke-ShvTest.ps1
```

`Invoke-ShvTest.ps1` prints one PASS/FAIL line per check and exits with a
non-zero code if any check fails, so you can use it in a loop or in CI:

```
Underlying hypervisor: VMwareVMware (...); WAITPKG before/loaded: 1/1 (ok)
  PASS  Guest test ran to completion
  PASS  SimpleVisor not detected before loading
  PASS  sc start simplevisor succeeded
  PASS  SimpleVisor detected while loaded
  ...
```

### Picking an edition

ISOs with several editions need the edition to install, given by name or by
index. `SERVERSTANDARD` is Server Standard with the Desktop Experience, and the
`...CORE` editions are Server Core. Server editions get the VMware guest OS type
for Windows Server 2025, and anything else gets Windows 11's. Retail
multi-edition ISOs may also need a key, and a generic installation key is
enough because it doesn't activate Windows:

```powershell
.\New-LabVm.ps1 -IsoPath D:\iso\Server2025_Eval.iso -ListImages
.\New-LabVm.ps1 -IsoPath D:\iso\Win11.iso -ImageIndex 6 -ProductKey <generic key>
```

### Hosts that run a hypervisor

If the host runs Hyper-V or VBS, VMware won't power on a VM that asks for
virtualized VT-x. It fails with "Virtualized Intel VT-x/EPT is not supported on
this platform", and its `vmware.log` shows `Monitor Mode: ULM`. `New-LabVm.ps1`
then stops. With `-SkipHostCheck` it creates the VM without virtualized VT-x
instead. Windows, kernel debugging and the test deployment still work, but
SimpleVisor can't load, because the guest has no VMX. `Invoke-ShvTest.ps1`
reports that. Recreate the VM once the host no longer runs a hypervisor, or use
the OpenVMM lab, which can nest VT-x on such hosts.

### Debugging

The guest has kernel debugging enabled on COM1 at 115200 baud. VMware connects
COM1 to the host pipe `\\.\pipe\SimpleVisorLab`, with the VM as the pipe server.
Before each test, `Invoke-ShvTest.ps1` checks that this works, unless you pass
`-SkipDebuggerCheck`. The check takes about 10 seconds:

1. It breaks into the guest by writing the KD break-in byte to the pipe. kd's
   own `-b` misses a running target too often over this pipe.
2. kd attaches, identifies the target (`vertarget`) and resumes it (`g`).
3. The check confirms with a guest operation that the guest runs again.

The guest test also checks the guest's BCD debug settings.

When SimpleVisor can't handle a VM-Exit, it prints the exit details and the
guest state to the debugger, saves them in the `Fatal*` fields of its
per-processor `SHV_VP_DATA`, and breaks in. On unload, it prints how many
VM-Exits it handled per exit reason. While it's loaded, `eulercheck.exe` also
reads these counters, through a magic CPUID leaf, and the test shows them as
`VM-Exits while loaded`. To catch the debugger output during a test, pass
`-AttachDebugger`. kd then stays attached in its own window, the test shows
its output at the end, and logs it to `out\kd-attached.log`. Or attach with:

```powershell
.\Connect-LabDebugger.ps1            # WinDbg
.\Connect-LabDebugger.ps1 -Console   # kd
.\Connect-LabDebugger.ps1 -Verify    # only check that kd can break in and resume
```

If the guest hangs during a test, `Invoke-ShvTest.ps1` leaves the VM running
for this. It also saves a screenshot to `out\guest-screen.png`. Use
`-KeepRunning` to keep the VM running after a passing test too, and `-Gui` to
watch it.

### The console

VMware serves the VM console over VNC on `127.0.0.1:5917`, which isn't
reachable from other machines and has no password. You can connect any VNC
viewer to it. The scripts use it to take screenshots, because `vmrun
captureScreen` needs VMware Tools running in a responsive guest. That rules it
out during the install, and after a hang or a bugcheck. Take one by hand with:

```powershell
. .\LabCommon.ps1; Save-LabScreenshot -Path .\out\screen.png
```

`New-LabVm.ps1` saves `out\install-screen.png` if the install doesn't finish
in time.

When neither VMware Tools nor the network works, `Send-LabKeys` types into the
console over the same connection, for example to open a PowerShell window:

```powershell
. .\LabCommon.ps1; Send-LabKeys -RunDialog -Text 'powershell' -Enter
```

## The OpenVMM lab

`openvmm\` has the same lab for [OpenVMM](https://github.com/microsoft/openvmm).
OpenVMM runs on the Windows Hypervisor Platform (WHP). With `--nested-virt` it
exposes VT-x to the guest, even when the host itself runs Hyper-V or VBS, which
is where VMware can't nest.

### Requirements

- An OpenVMM build next to this repo, in `..\openvmm`, or set
  `SHV_OPENVMM_ROOT`. The lab uses these files from it:
  - `target\release\openvmm.exe`, or set `SHV_OPENVMM_EXE`.
  - The `mscoreuefi` UEFI firmware (`MSVM.fd`) from the repo's `.packages`.
  - The virtio-win NetKVM driver for Windows Server 2025 from the repo's
    `.packages`.
- The Windows Hypervisor Platform feature on the host.
- A Windows Server 2025 ISO, because the NIC driver is the one for Server 2025.
- The same tools as the VMware lab: the EWDK and the ADK's `oscdimg.exe`.

### Usage

```powershell
cd vmlab\openvmm

# Once: create the VM. This takes about 15-30 minutes and is fully unattended.
.\New-OvmmLabVm.ps1 -IsoPath D:\iso\Server2025_Eval.iso -ImageName 'Windows Server 2025 SERVERSTANDARD'

# Every time: build, deploy, load, check, unload.
.\Invoke-OvmmShvTest.ps1

# Attach the debugger.
..\Connect-LabDebugger.ps1 -OpenVmm
```

### How it differs from the VMware lab

- **Two configurations.** WHP doesn't allow nested virtualization together with
  VMBus. So Windows is installed with VMBus, using the install DVD on VMBus SCSI
  and synthetic video, and tested without VMBus, with VT-x exposed. Both
  configurations attach the same NVMe disk and virtio NIC over PCIe, so Windows
  sees the same devices in both.
- **No snapshots.** OpenVMM can't save and restore a VM with nested
  virtualization or NVMe. Instead, the installed disk is kept read-only as the
  clean state. Each test boots a throwaway in-memory diff of it (`memdiff:`),
  which is a cold boot of about 3 minutes.
- **SSH instead of VMware Tools.** The first logon installs the virtio NIC
  driver, which Windows doesn't have inbox, and turns on the OpenSSH server. It
  accepts only a key generated per lab. OpenVMM's NAT forwards the guest's SSH
  port to `127.0.0.1:22222`.
- **The console.** VNC is on `127.0.0.1:5931`. Without VMBus there's no
  synthetic video, so the console is black during tests, and only the debugger
  shows what the guest is doing. For `Send-LabKeys`, pass `-Port 5931
  -ScancodeWinKey`.
- **The debugger pipe** is `\\.\pipe\SimpleVisorLabOvmm`. It's a plain pipe, not
  in OpenVMM's `debugger-mode`. That mode delays the guest's reads of an empty
  serial port, and Windows KD waits for data in exactly such loops, so kd stalls
  for minutes after connecting, and boot is three times slower.
- **The lab** lives in `Documents\Virtual Machines\SimpleVisorLabOvmm`: the
  disk, the SSH key, and OpenVMM's logs and output.

## The Hyper-V lab

`hyperv\` has the same lab for Hyper-V. Hyper-V exposes VT-x to the guest with
nested virtualization, so SimpleVisor runs under the real Hyper-V, with VMBus
and its synthetic devices. The guest sees Hyper-V underneath (`Hv#1`).

### Requirements

- The Hyper-V feature on the host, with its PowerShell module.
- Membership in the Hyper-V Administrators group. The scripts need no elevated
  prompt then, not even for PowerShell Direct or the debugger pipe.
- The same tools as the VMware lab: the EWDK and the ADK's `oscdimg.exe`.

### Usage

```powershell
cd vmlab\hyperv

# Once: create the VM. This takes about 10-20 minutes and is fully unattended.
.\New-HvLabVm.ps1 -IsoPath D:\iso\Server2025_Eval.iso -ImageName 'Windows Server 2025 SERVERSTANDARD'

# Every time: build, deploy, load, check, unload.
.\Invoke-HvShvTest.ps1

# Attach the debugger.
..\Connect-LabDebugger.ps1 -HyperV
```

### How it differs from the VMware lab

- **The VM** is `SimpleVisorLabHv`, a generation 2 VM with Secure Boot off,
  nested virtualization, 4 vCPUs, 4 GB of static memory (nested virtualization
  rules out dynamic memory) and a 64 GB VHDX. It has no NIC.
- **PowerShell Direct instead of VMware Tools.** The host runs commands and
  copies files over VMBus, as the guest's Administrator, so the guest needs no
  network and no setup. A command that doesn't return in time, e.g. because
  SimpleVisor halted the guest, counts as the guest having stopped responding.
- **The checkpoint** `clean` is taken while the VM is off, as a VM with nested
  virtualization can't be saved. Each test therefore reverts to it and cold
  boots.
- **The console.** There's no VNC. Screenshots come from Hyper-V's thumbnail of
  the console, so they work after a hang or a bugcheck too, and `Send-HvKeys`
  types into it. `vmconnect localhost SimpleVisorLabHv` shows it live.

  ```powershell
  . .\HvLab.ps1; Save-HvScreenshot -Path .\screen.png
  ```
- **The debugger pipe** is `\\.\pipe\SimpleVisorLabHv`, on COM1. Hyper-V is the
  pipe server, while the VM runs.
- **The lab** lives in `Documents\Virtual Machines\SimpleVisorLabHv`: the VM's
  configuration, its disk and checkpoint, and the test's results and logs.

## What the scripts do

| Script | Purpose |
| --- | --- |
| `New-LabVm.ps1` | Creates the VM from the ISO, installs Windows unattended, and takes the `clean` snapshot. |
| `Build-LabArtifacts.ps1` | Builds `euler.sys` and `eulercheck.exe` with the EWDK, and test-signs `euler.sys`. |
| `Invoke-ShvTest.ps1` | Reverts to `clean`, copies the files in, runs the guest test, and checks the results. |
| `Connect-LabDebugger.ps1` | Attaches WinDbg or kd to the VM's serial pipe (`-OpenVmm` for the OpenVMM VM, `-HyperV` for the Hyper-V one). |
| `LabCommon.ps1` | Shared settings (VM name and location, guest credentials) and helpers: install media, kd, VNC, result checks. |
| `openvmm\New-OvmmLabVm.ps1` | Creates the OpenVMM VM's clean disk from the ISO, installing Windows unattended. |
| `openvmm\Invoke-OvmmShvTest.ps1` | Boots a diff of the clean disk with VT-x exposed, and runs the same test as `Invoke-ShvTest.ps1`. |
| `openvmm\OvmmLab.ps1` | OpenVMM settings and helpers: command lines, start and stop, SSH. |
| `hyperv\New-HvLabVm.ps1` | Creates the Hyper-V VM from the ISO, installs Windows unattended, and takes the `clean` checkpoint. |
| `hyperv\Invoke-HvShvTest.ps1` | Reverts to `clean`, and runs the same test as `Invoke-ShvTest.ps1` over PowerShell Direct. |
| `hyperv\HvLab.ps1` | Hyper-V settings and helpers: PowerShell Direct, screenshots, typing into the console. |
| `unattend\autounattend.xml` | The Windows Setup answer file template. |
| `guest\FirstLogon.ps1` | Runs once in the new guest to set it up for SimpleVisor, and for VMware Tools, SSH or PowerShell Direct. |
| `guest\Test-ShvGuest.ps1` | Runs in the guest for each test: load, probe, unload. |
| `guest\eulercheck.c` | The guest probe, which reports the CPUID state that SimpleVisor affects, and its VM-Exit counters. |

### The VM

The VM is created in `Documents\Virtual Machines\SimpleVisorLab`. It has these
settings:

- UEFI firmware with Secure Boot off, so test signing works.
- Virtualized VT-x/EPT (`vhv.enable`).
- 4 vCPUs, 4 GB of RAM and a 64 GB NVMe disk.
- NAT networking.
- COM1 connected to the debugger pipe.
- The console on VNC, on the loopback interface only.

Windows Setup can't run unattended from the stock ISO, because the UEFI boot
waits at a "Press any key to boot from CD or DVD" prompt. So `New-LabVm.ps1`
rebuilds the ISO with the no-prompt boot image, the answer file and the
first-logon script.

### The guest

The answer file installs Windows on a wiped disk and bypasses the Windows 11
TPM and Secure Boot checks. It turns on automatic logon for the built-in
Administrator account. On first logon, `FirstLogon.ps1` makes these changes:

- Turns on test signing and kernel debugging.
- Turns off the hypervisor and VBS. SimpleVisor can't load under Hyper-V
  running inside the guest.
- Keeps bugchecks on screen instead of rebooting.
- Turns off sleep and automatic Windows Update.
- Installs VMware Tools, which `vmrun` needs to drive the guest. In the OpenVMM
  lab it installs the virtio NIC driver and the OpenSSH server instead. The
  Hyper-V lab needs neither, as PowerShell Direct is inbox.
- Reboots.

### Credentials

The guest's `Administrator` and `lab` accounts use the password set in
`LabCommon.ps1` (`SimpleVisor!Lab1`), or the one in the `SHV_LAB_PASSWORD`
environment variable while creating the VM, which the lab scripts then need as
well. The VM is meant to be a throwaway lab machine only.

The lab test-signing certificate is created on first use in the current user's
certificate store (`Cert:\CurrentUser\My`), with the subject
`CN=SimpleVisor Lab Test Signing`. It is trusted only inside the guest.
