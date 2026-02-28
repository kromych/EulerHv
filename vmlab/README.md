# SimpleVisor VMware lab

These scripts build a VMware Workstation VM for loading SimpleVisor, unloading
it, and checking what the guest sees while it's loaded. When SimpleVisor halts
or the guest bugchecks, you can debug it. Because the VM has VMware underneath,
the lab also exercises SimpleVisor's nested paths: VMware detection, passing the
hypervisor CPUID leaves through, and the EPT limits.

## Requirements

- **VMware Workstation Pro 17** or newer (free for personal and commercial use).
- **No hypervisor on the host.** VMware can only give a guest VT-x ("Virtualize
  Intel VT-x/EPT") when it owns VT-x itself. If Hyper-V runs on the host, VMware
  runs on top of it and can't nest, and SimpleVisor has no VT-x to load on. VBS
  features count too, such as Memory Integrity and Credential Guard. The scripts
  check this and say what to turn off.
- **An Intel CPU.** SimpleVisor is VT-x only.
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
reports that. Recreate the VM once the host no longer runs a hypervisor.

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

When SimpleVisor can't handle a VM-Exit, it saves the exit details in the
`Fatal*` fields of its per-processor `SHV_VP_DATA` and breaks into the
debugger. Attach with:

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

## What the scripts do

| Script | Purpose |
| --- | --- |
| `New-LabVm.ps1` | Creates the VM from the ISO, installs Windows unattended, and takes the `clean` snapshot. |
| `Build-LabArtifacts.ps1` | Builds `shv.sys` and `shvcheck.exe` with the EWDK, and test-signs `shv.sys`. |
| `Invoke-ShvTest.ps1` | Reverts to `clean`, copies the files in, runs the guest test, and checks the results. |
| `Connect-LabDebugger.ps1` | Attaches WinDbg or kd to the VM's serial pipe. |
| `LabCommon.ps1` | Shared settings (VM name and location, guest credentials) and helpers. |
| `unattend\autounattend.xml` | The Windows Setup answer file template. |
| `guest\FirstLogon.ps1` | Runs once in the new guest to set it up for SimpleVisor. |
| `guest\Test-ShvGuest.ps1` | Runs in the guest for each test: load, probe, unload. |
| `guest\shvcheck.c` | The guest probe, which reports the CPUID state that SimpleVisor affects. |

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
- Installs VMware Tools, which `vmrun` needs to drive the guest.
- Reboots.

### Credentials

The guest's `Administrator` and `lab` accounts use the password set in
`LabCommon.ps1` (`SimpleVisor!Lab1`). The VM is meant to be a throwaway lab
machine only.

The lab test-signing certificate is created on first use in the current user's
certificate store (`Cert:\CurrentUser\My`), with the subject
`CN=SimpleVisor Lab Test Signing`. It is trusted only inside the guest.
