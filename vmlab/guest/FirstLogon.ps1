#
# Runs once, elevated, at the first logon of the freshly installed lab VM (see
# unattend\autounattend.xml). It prepares the guest for loading SimpleVisor,
# installs VMware Tools so the host can drive the guest with vmrun, and reboots.
# After that reboot, a RunOnce entry writes C:\vmlab\ready.txt, which tells
# New-LabVm.ps1 that the configuration is in effect.
#

$ErrorActionPreference = 'Continue'
$labDir = 'C:\vmlab'
New-Item -ItemType Directory -Path $labDir -Force | Out-Null
Start-Transcript -Path (Join-Path $labDir 'firstlogon.log') -Append | Out-Null

#
# SimpleVisor is test-signed, so test signing must be on. This requires Secure
# Boot to be off, which the VM configuration takes care of.
#
bcdedit /set testsigning on

#
# Enable kernel debugging over COM1, which the VM connects to the named pipe
# \\.\pipe\SimpleVisorLab on the host. SimpleVisor breaks into the debugger
# when it halts, so this is the way to see why.
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
# Keep a bugcheck on screen instead of rebooting, so that it can be seen (or
# captured with "vmrun captureScreen"), and never sleep.
#
Set-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl' -Name AutoReboot -Value 0 -Type DWord
powercfg /hibernate off
powercfg /change standby-timeout-ac 0
powercfg /change monitor-timeout-ac 0

#
# Keep Windows Update from rebooting the VM, or changing it between snapshots
#
$wu = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate\AU'
New-Item -Path $wu -Force | Out-Null
Set-ItemProperty -Path $wu -Name NoAutoUpdate -Value 1 -Type DWord

#
# Install VMware Tools from its CD, which the VM has attached next to the
# install ISO. Older Tools ISOs ship setup64.exe, and newer ones only setup.exe,
# so recognize the CD by its manifest instead. Suppress the installer's reboot;
# we reboot below anyway. Wait for the installer process itself only, unlike
# Start-Process -Wait, which also waits for anything it leaves running.
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
}
else {
    Write-Warning 'The VMware Tools CD was not found; the host cannot drive this guest.'
}

#
# Signal readiness only once the settings above are in effect, after a reboot
#
Set-ItemProperty -Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\RunOnce' `
                 -Name SimpleVisorLabReady `
                 -Value "cmd /c echo ready> $labDir\ready.txt"

Stop-Transcript | Out-Null
Restart-Computer -Force
