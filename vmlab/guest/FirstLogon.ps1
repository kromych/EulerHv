#
# Runs once, elevated, at the first logon of the freshly installed lab VM (see
# unattend\autounattend.xml). It prepares the guest for loading SimpleVisor,
# sets up the channel the host drives the guest through, and reboots. After
# that reboot, a RunOnce entry writes C:\vmlab\ready.txt, which tells the host
# that the configuration is in effect. If a setting the lab needs fails, it
# neither reboots nor marks the VM ready, which leaves it for a look at
# C:\vmlab\firstlogon.log, rather than making a broken VM the clean snapshot.
#
# The channel depends on the lab, as told by what the install media carries:
#   - VMware: VMware Tools, from the Tools CD attached to the VM, for vmrun
#   - OpenVMM: the virtio NIC driver (vmlab\netkvm) and the OpenSSH server,
#     with the lab's key (vmlab\id_lab.pub), for ssh and scp
#   - Hyper-V: nothing, as PowerShell Direct is inbox (vmlab\hyperv.txt)
#

#
# All of this reconfigures the machine and reboots it, so only go on from the
# lab's install ISO, which has autounattend.xml at its root, next to vmlab
#
if (-not (Test-Path (Join-Path (Split-Path $PSScriptRoot -Parent) 'autounattend.xml'))) {
    throw 'FirstLogon.ps1 only runs at the first logon of a lab VM, from its install ISO.'
}

$ErrorActionPreference = 'Continue'
$labDir = 'C:\vmlab'
New-Item -ItemType Directory -Path $labDir -Force | Out-Null
Start-Transcript -Path (Join-Path $labDir 'firstlogon.log') -Append | Out-Null
$channel = $false

#
# The settings that tests would otherwise fail on without saying why. The
# channel needs no check: without it the host never sees the VM ready
#
$failures = @()
function Invoke-Bcdedit {
    bcdedit @args
    if ($LASTEXITCODE -ne 0) {
        $script:failures += "bcdedit $args exited with $LASTEXITCODE"
    }
}

#
# SimpleVisor is test-signed, so test signing must be on. This requires Secure
# Boot to be off, which the VM configuration takes care of.
#
Invoke-Bcdedit /set testsigning on

#
# Enable kernel debugging over COM1, which the VM connects to a named pipe on
# the host. SimpleVisor breaks into the debugger when it halts, so this is the
# way to see why.
#
Invoke-Bcdedit /debug on
Invoke-Bcdedit /dbgsettings serial debugport:1 baudrate:115200

#
# SimpleVisor cannot load if Windows itself runs under Hyper-V, which it does
# whenever VBS (e.g. Memory Integrity) is on. Keep the hypervisor off.
#
Invoke-Bcdedit /set hypervisorlaunchtype off

#
# Force x2APIC, which the flush IPIs need. Windows enables it at boot when
# the processor offers it, and this policy makes it do so whenever it can
#
Invoke-Bcdedit /set x2apicpolicy Enable

#
# The platform must expose x2APIC, which the BCD policy cannot conjure up.
# The creation scripts put eulercheck.exe on the install media next to this
# script, and it reports the CPUID bit
#
$eulercheck = Join-Path $PSScriptRoot 'eulercheck.exe'
if (-not (Test-Path $eulercheck)) {
    $failures += 'eulercheck.exe was not on the install media; run Build-LabArtifacts.ps1 before creating the VM'
}
else {
    $x2apic = & $eulercheck | Where-Object { $_ -match '^x2apic_cpuid=' }
    if ($x2apic -ne 'x2apic_cpuid=1') {
        $failures += "The platform does not expose x2APIC to the guest (eulercheck said '$x2apic')"
    }
}
$deviceGuard = 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard'
try {
    New-Item -Path "$deviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity" -Force -ErrorAction Stop | Out-Null
    Set-ItemProperty -Path $deviceGuard -Name EnableVirtualizationBasedSecurity -Value 0 -Type DWord -ErrorAction Stop
    Set-ItemProperty -Path "$deviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity" -Name Enabled -Value 0 -Type DWord -ErrorAction Stop
}
catch {
    $failures += "Turning off VBS failed: $_"
}

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

    #
    # sshd also takes passwords by default. Its first start wrote the default
    # configuration, where the first value of a setting wins, so turn them off
    # at the top, ahead of the Match block at the end
    #
    $config = 'C:\ProgramData\ssh\sshd_config'
    try {
        Set-Content -Path $config -Encoding Ascii -Value (@('PasswordAuthentication no') + @(Get-Content -Path $config -ErrorAction Stop)) -ErrorAction Stop
        Restart-Service sshd -ErrorAction Stop
    }
    catch {
        $failures += "Turning off sshd password logons failed: $_"
    }
    if (-not (Get-NetFirewallRule -Name 'OpenSSH-Server-In-TCP' -ErrorAction SilentlyContinue)) {
        New-NetFirewallRule -Name 'OpenSSH-Server-In-TCP' -DisplayName 'OpenSSH Server (sshd)' -Enabled True `
                            -Direction Inbound -Protocol TCP -Action Allow -LocalPort 22 | Out-Null
    }
    Set-NetFirewallRule -Name 'OpenSSH-Server-In-TCP' -Enabled True -Profile Any
    $channel = $true
}

#
# For Hyper-V (vmlab\hyperv.txt): PowerShell Direct, which is inbox
#
if (Test-Path (Join-Path $PSScriptRoot 'hyperv.txt')) {
    $channel = $true
}

if (-not $channel) {
    Write-Warning 'Neither the VMware Tools CD, the OpenSSH key, nor PowerShell Direct was found; the host cannot drive this guest.'
}

#
# Leave a VM that misses a setting as it is, without rebooting or marking it
# ready. The host gives up waiting for it, and points at this log
#
if ($failures) {
    Write-Warning "The VM is not marked ready, as setting it up failed:`n$($failures -join "`n")"
    Stop-Transcript | Out-Null
    exit 1
}

#
# Signal readiness only once the settings above are in effect, after a reboot
#
Set-ItemProperty -Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\RunOnce' `
                 -Name SimpleVisorLabReady `
                 -Value "cmd /c echo ready> $labDir\ready.txt"

Stop-Transcript | Out-Null
Restart-Computer -Force
