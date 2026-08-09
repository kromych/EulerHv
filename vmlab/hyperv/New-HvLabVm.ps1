<#
.SYNOPSIS
    Creates the Hyper-V VM used to exercise SimpleVisor nested under Hyper-V;
    the counterpart of ..\New-LabVm.ps1.

.DESCRIPTION
    Remasters the given Windows install ISO into an unattended one, creates a
    generation 2 VM with nested virtualization, installs Windows, configures it
    for loading a test-signed hypervisor, and takes the checkpoint that
    Invoke-HvShvTest.ps1 reverts to before every run.

    The whole process is unattended and takes roughly 15-30 minutes. It needs
    membership in the Hyper-V Administrators group, but no elevation.

.PARAMETER IsoPath
    A 64-bit Windows 10, Windows 11 or Windows Server ISO. Evaluation media is
    easiest, as it needs no product key.

.PARAMETER ImageIndex
    The index of the edition in install.wim (or install.esd) to install. Use
    -ListImages to see them.

.PARAMETER ImageName
    The name of the edition to install, as an alternative to -ImageIndex, e.g.
    "Windows Server 2025 SERVERSTANDARD" (with the Desktop Experience).

.PARAMETER ProductKey
    The product key, if the media needs one to pick an edition. A generic
    installation key, which does not activate Windows, is enough.

.PARAMETER ListImages
    List the editions on the ISO and exit.

.EXAMPLE
    .\New-HvLabVm.ps1 -IsoPath D:\iso\Server2025_Eval.iso -ImageName 'Windows Server 2025 SERVERSTANDARD'
#>
[CmdletBinding()]
param (
    [Parameter(Mandatory)] [string] $IsoPath,
    [int] $ImageIndex = 1,
    [string] $ImageName,
    [string] $ProductKey,
    [switch] $ListImages,
    [int] $DiskGB = 64,
    [string] $Locale = 'en-US',
    [string] $OscdimgPath,
    [int] $InstallTimeoutMin = 120,
    [switch] $Force
)

. (Join-Path $PSScriptRoot 'HvLab.ps1')

$IsoPath = (Resolve-Path $IsoPath).Path
if ($ListImages) {
    Get-IsoImage -Path $IsoPath | Format-Table ImageIndex, ImageName -AutoSize
    return
}

Assert-HvPrerequisite
$image = Resolve-LabImage -IsoPath $IsoPath -ImageIndex $ImageIndex -ImageName $ImageName
Write-Host "Installing '$($image.ImageName)' (index $($image.ImageIndex))."

$vmName = $HvDefaults.VmName
$labDir = $HvDefaults.LabDir
if ((Get-HvLabVm) -or (Test-Path $labDir)) {
    if (-not $Force) {
        throw "The VM $vmName or $labDir already exists. Pass -Force to delete and recreate the VM."
    }
    Stop-HvLabVm
    Get-HvLabVm | Remove-VM -Force
    Remove-Item -Path $labDir -Recurse -Force -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Path $labDir | Out-Null

#
# The marker tells FirstLogon.ps1 that the host drives this guest through
# PowerShell Direct, which needs no setup in the guest
#
$marker = Get-HvLabFile 'hyperv.txt'
Set-Content -Path $marker -Value 'The host drives this guest through PowerShell Direct.'

#
# eulercheck.exe goes on the install media too, where FirstLogon.ps1 runs it
# to enforce x2APIC, which the flush IPIs need
#
$eulercheck = Join-Path $LabDefaults.OutDir 'eulercheck.exe'
if (-not (Test-Path $eulercheck)) {
    throw "eulercheck.exe was not built to $($LabDefaults.OutDir). Run .\Build-LabArtifacts.ps1 first."
}

$installIso = Get-HvLabFile 'install.iso'
New-UnattendedIso -SourceIso $IsoPath -DestinationIso $installIso -ImageIndex $image.ImageIndex `
                  -ComputerName $vmName -ProductKey $ProductKey -Locale $Locale -OscdimgPath $OscdimgPath `
                  -ExtraFiles @(@{ Source = $marker; Destination = 'hyperv.txt' },
                                @{ Source = $eulercheck; Destination = 'eulercheck.exe' })
Remove-Item -Path $marker

#
# Notable settings:
#   generation 2            UEFI, with Secure Boot off, so test signing works
#   static memory           nested virtualization rules out dynamic memory
#   nested virtualization   expose VT-x, so SimpleVisor can load
#   no CPU compatibility    the compatibility modes strip CPU features such
#                           as x2APIC, which the flush IPIs need
#   COM1                    on a named pipe, for the kernel debugger. Hyper-V
#                           is the pipe server, and the debugger the client.
#   no NIC                  PowerShell Direct goes over VMBus, so the guest
#                           needs no network
#   checkpoints             standard ones, taken only while the VM is off, as
#                           a VM with nested virtualization can't be saved,
#                           and no automatic ones
#
# The disk comes first in the boot order. While it is still empty, the
# firmware falls through to the DVD, and once Windows is installed, it boots
# from the disk rather than starting Setup over.
#
Write-Host "Creating the VM $vmName..."
$disk = Get-HvLabFile 'disk.vhdx'
$vm = New-VM -Name $vmName -Generation 2 -Path $LabDefaults.VmRoot -NoVHD -MemoryStartupBytes ([int64] $HvDefaults.MemoryMB * 1MB)
Set-VM -VM $vm -StaticMemory -CheckpointType Standard -AutomaticCheckpointsEnabled $false `
       -AutomaticStopAction TurnOff -AutomaticStartAction Nothing
Set-VMProcessor -VM $vm -Count $HvDefaults.CpuCount -ExposeVirtualizationExtensions $true `
                -CompatibilityForMigrationEnabled $false -CompatibilityForOlderOperatingSystemsEnabled $false
Get-VMNetworkAdapter -VM $vm | Remove-VMNetworkAdapter
New-VHD -Path $disk -SizeBytes ([int64] $DiskGB * 1GB) -Dynamic | Out-Null
$hdd = Add-VMHardDiskDrive -VM $vm -Path $disk -Passthru
$dvd = Add-VMDvdDrive -VM $vm -Path $installIso -Passthru
Set-VMFirmware -VM $vm -EnableSecureBoot Off -BootOrder $hdd, $dvd
Set-VMComPort -VM $vm -Number 1 -Path "\\.\pipe\$($HvDefaults.PipeName)"

Write-Host "Starting $vmName and installing Windows; this takes a while..."
Start-VM -VM $vm
try {
    Wait-HvGuest -TimeoutSec ($InstallTimeoutMin * 60)
}
catch {
    $screenshot = Get-HvLabFile 'install-screen.png'
    try { Save-HvScreenshot -Path $screenshot } catch { $screenshot = "<none: $_>" }
    throw "$_ The VM is left running; see $screenshot for its console, and C:\vmlab\firstlogon.log in the guest."
}

#
# Shut down cleanly, drop the install media, which is not needed anymore, and
# take the checkpoint every test run starts from
#
Write-Host 'Windows is installed and configured; taking the checkpoint...'
Stop-VM -VM $vm -Force
Remove-VMDvdDrive -VMDvdDrive $dvd
Remove-Item -Path $installIso
Checkpoint-VM -VM $vm -SnapshotName $LabDefaults.Snapshot

Write-Host "Created $vmName with checkpoint '$($LabDefaults.Snapshot)'. Run .\Invoke-HvShvTest.ps1 next."
