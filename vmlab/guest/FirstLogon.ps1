#
# Runs once, elevated, at the first logon of the freshly installed lab VM (see
# unattend\autounattend.xml). It prepares the guest for loading SimpleVisor,
# sets up the channel the host drives the guest through, and reboots. After
# that reboot, a RunOnce entry writes C:\vmlab\ready.txt, which tells the host
# that the configuration is in effect.
#
# The channel depends on the lab, as told by what the install media carries:
#   - VMware: VMware Tools, from the Tools CD attached to the VM, for vmrun
#   - OpenVMM: the virtio NIC driver (vmlab\netkvm) and the OpenSSH server,
#     with the lab's key (vmlab\id_lab.pub), for ssh and scp
#

$ErrorActionPreference = 'Continue'
$labDir = 'C:\vmlab'
New-Item -ItemType Directory -Path $labDir -Force | Out-Null
Start-Transcript -Path (Join-Path $labDir 'firstlogon.log') -Append | Out-Null
$channel = $false

#
# SimpleVisor is test-signed, so test signing must be on. This requires Secure
# Boot to be off, which the VM configuration takes care of.
#
bcdedit /set testsigning on

#
# Enable kernel debugging over COM1, which the VM connects to a named pipe on
# the host. SimpleVisor breaks into the debugger when it halts, so this is the
# way to see why.
#
bcdedit /debug on
bcdedit /dbgsettings serial debugport:1 baudrate:115200

#
# SimpleVisor cannot load if Windows itself runs under Hyper-V, which it does
# whenever VBS (e.g. Memory Integrity) is on. Keep the hypervisor off.
#
bcdedit /set hypervisorlaunchtype off
$deviceGuard = 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard'
New-Item -Path "$deviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity" -Force | Out-Null
Set-ItemProperty -Path $deviceGuard -Name EnableVirtualizationBasedSecurity -Value 0 -Type DWord
Set-ItemProperty -Path "$deviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity" -Name Enabled -Value 0 -Type DWord

#
# Keep a bugcheck on screen instead of rebooting, so that it can be seen in a
# screenshot, and never sleep.
#
Set-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl' -Name AutoReboot -Value 0 -Type DWord
powercfg /hibernate off
powercfg /change standby-timeout-ac 0
powercfg /change monitor-timeout-ac 0

#
# Keep Windows Update from rebooting the VM, or changing it between snapshots,
# and Server Manager from popping up at every logon
#
$wu = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate\AU'
New-Item -Path $wu -Force | Out-Null
Set-ItemProperty -Path $wu -Name NoAutoUpdate -Value 1 -Type DWord
Get-ScheduledTask -TaskName ServerManager -ErrorAction SilentlyContinue | Disable-ScheduledTask | Out-Null

#
# For VMware: install VMware Tools from its CD, which the VM has attached next
# to the install ISO. Older Tools ISOs ship setup64.exe, and newer ones only
# setup.exe, so recognize the CD by its manifest instead. Suppress the
# installer's reboot; we reboot below anyway. Wait for the installer process
# itself only, unlike Start-Process -Wait, which also waits for anything it
# leaves running.
#
$toolsCd = Get-PSDrive -PSProvider FileSystem |
           Where-Object { (Test-Path (Join-Path $_.Root 'manifest.txt')) -and
                          (Test-Path (Join-Path $_.Root 'VMwareToolsUpgrader.exe')) } |
           Select-Object -First 1
$tools = $null
if ($toolsCd) {
    $tools = 'setup64.exe', 'setup.exe' |
             ForEach-Object { Join-Path $toolsCd.Root $_ } |
             Where-Object { Test-Path $_ } |
             Select-Object -First 1
}
if ($tools) {
    $installer = Start-Process -FilePath $tools -ArgumentList '/S /v "/qn REBOOT=R"' -PassThru
    $installer.WaitForExit()
    Write-Output "VMware Tools setup ($tools) exited with $($installer.ExitCode)"
    $channel = $true
}

#
# For OpenVMM: its NIC is virtio-net on PCIe, as OpenVMM cannot offer VMBus
# (netvsc) while it exposes nested virtualization, and Windows has no inbox
# virtio driver.
#
$netKvm = Join-Path $PSScriptRoot 'netkvm\netkvm.inf'
if (Test-Path $netKvm) {
    pnputil /add-driver $netKvm /install
}

#
# For OpenVMM: the OpenSSH server, for running commands and copying files from
# the host. The lab key is the only way in for administrators. The built-in
# firewall rule only covers the Private profile, while OpenVMM's NAT network
# comes up as Public.
#
$labKey = Join-Path $PSScriptRoot 'id_lab.pub'
if (Test-Path $labKey) {
    $capability = Get-WindowsCapability -Online -Name 'OpenSSH.Server*' | Select-Object -First 1
    if ($capability -and ($capability.State -ne 'Installed')) {
        Add-WindowsCapability -Online -Name $capability.Name | Out-Null
    }
    Set-Service -Name sshd -StartupType Automatic
    Start-Service sshd
    $keys = 'C:\ProgramData\ssh\administrators_authorized_keys'
    Copy-Item -Path $labKey -Destination $keys -Force
    icacls.exe $keys /inheritance:r /grant 'Administrators:F' /grant 'SYSTEM:F' | Out-Null
    if (-not (Get-NetFirewallRule -Name 'OpenSSH-Server-In-TCP' -ErrorAction SilentlyContinue)) {
        New-NetFirewallRule -Name 'OpenSSH-Server-In-TCP' -DisplayName 'OpenSSH Server (sshd)' -Enabled True `
                            -Direction Inbound -Protocol TCP -Action Allow -LocalPort 22 | Out-Null
    }
    Set-NetFirewallRule -Name 'OpenSSH-Server-In-TCP' -Enabled True -Profile Any
    $channel = $true
}

if (-not $channel) {
    Write-Warning 'Neither the VMware Tools CD nor the OpenSSH key was found; the host cannot drive this guest.'
}

#
# Signal readiness only once the settings above are in effect, after a reboot
#
Set-ItemProperty -Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\RunOnce' `
                 -Name SimpleVisorLabReady `
                 -Value "cmd /c echo ready> $labDir\ready.txt"

Stop-Transcript | Out-Null
Restart-Computer -Force
