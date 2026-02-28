<#
.SYNOPSIS
    Creates the OpenVMM VM used to exercise SimpleVisor; the counterpart of
    ..\New-LabVm.ps1, for hosts where VMware cannot nest VT-x.

.DESCRIPTION
    Remasters the given Windows install ISO into an unattended one, which also
    carries the virtio NIC driver and the lab's SSH key, creates a sparse raw
    disk, and installs Windows in OpenVMM's "install" configuration. Once the
    guest is configured, it is shut down, and the disk is made read-only: it is
    the clean state that Invoke-OvmmShvTest.ps1 boots a throwaway in-memory diff
    of, with VT-x exposed.

    The lab lives in Documents\Virtual Machines\SimpleVisorLabOvmm. The whole
    process is unattended and takes roughly 15-30 minutes.

.PARAMETER IsoPath
    A 64-bit Windows Server 2025 (or Windows 11 24H2) ISO; the virtio NIC driver
    used is the one for Windows Server 2025. Evaluation media is easiest, as it
    needs no product key.

.PARAMETER ImageIndex
    The index of the edition in install.wim (or install.esd) to install. Use
    -ListImages to see them.

.PARAMETER ImageName
    The name of the edition to install, as an alternative to -ImageIndex, e.g.
    "Windows Server 2025 SERVERSTANDARD" (with the Desktop Experience).

.PARAMETER ListImages
    List the editions on the ISO and exit.

.EXAMPLE
    .\New-OvmmLabVm.ps1 -IsoPath D:\iso\Server2025_Eval.iso -ImageName 'Windows Server 2025 SERVERSTANDARD'
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

. (Join-Path $PSScriptRoot 'OvmmLab.ps1')

$IsoPath = (Resolve-Path $IsoPath).Path
if ($ListImages) {
    Get-IsoImage -Path $IsoPath | Format-Table ImageIndex, ImageName -AutoSize
    return
}

Assert-OvmmPrerequisite
$image = Resolve-LabImage -IsoPath $IsoPath -ImageIndex $ImageIndex -ImageName $ImageName
Write-Host "Installing '$($image.ImageName)' (index $($image.ImageIndex))."

$labDir = $OvmmDefaults.LabDir
if (Test-Path $labDir) {
    if (-not $Force) {
        throw "$labDir already exists. Pass -Force to delete and recreate the VM."
    }
    Stop-OvmmLabVm
    Get-ChildItem -Path $labDir -Recurse -File | ForEach-Object { $_.IsReadOnly = $false }
    Remove-Item -Path $labDir -Recurse -Force
}
New-Item -ItemType Directory -Path $labDir | Out-Null

#
# The key the host logs into the guest with, over SSH. It is only good for this
# lab VM, which only listens on the host's loopback interface.
#
$key = Get-OvmmLabFile 'id_lab'
Invoke-NativeCommand -FilePath "$env:SystemRoot\System32\OpenSSH\ssh-keygen.exe" `
                     -Arguments @('-q', '-t', 'ed25519', '-N', '', '-C', $OvmmDefaults.LabName, '-f', $key) | Out-Null

$installIso = Get-OvmmLabFile 'install.iso'
New-UnattendedIso -SourceIso $IsoPath -DestinationIso $installIso -ImageIndex $image.ImageIndex `
                  -ComputerName $OvmmDefaults.LabName -ProductKey $ProductKey -Locale $Locale -OscdimgPath $OscdimgPath `
                  -ExtraFiles @(@{ Source = $OvmmDefaults.NetKvm; Destination = 'netkvm' },
                                @{ Source = "$key.pub"; Destination = 'id_lab.pub' })

#
# A sparse raw disk: OpenVMM cannot create VHDX files, and a fixed VHD or a
# plain raw file would allocate all of it up front
#
$disk = Get-OvmmLabFile 'disk.img'
Write-Host "Creating the $DiskGB GB sparse disk $disk..."
[System.IO.File]::WriteAllBytes($disk, [byte[]] @())
Invoke-NativeCommand -FilePath 'fsutil.exe' -Arguments @('sparse', 'setflag', $disk) | Out-Null
$stream = [System.IO.File]::Open($disk, 'Open', 'ReadWrite')
try {
    $stream.SetLength([int64] $DiskGB * 1GB)
}
finally {
    $stream.Dispose()
}

Write-Host 'Starting OpenVMM and installing Windows; this takes a while...'
$vm = Start-OvmmLabVm -Mode install
try {
    Wait-OvmmGuest -Process $vm -TimeoutSec ($InstallTimeoutMin * 60)
}
catch {
    $screenshot = Get-OvmmLabFile 'install-screen.png'
    try { Save-LabScreenshot -Path $screenshot -Port $OvmmDefaults.VncPort } catch { $screenshot = "<none: $_>" }
    throw "$_ OpenVMM (PID $($vm.Id)) is left running; see $screenshot for its console, and C:\vmlab\firstlogon.log in the guest."
}

#
# Shut down cleanly, drop the install media, which is not needed anymore, and
# keep the disk as the clean state that every test run starts from
#
Write-Host 'Windows is installed and configured; shutting it down...'
Invoke-OvmmGuest -Command 'shutdown /s /f /t 0' -AllowFailure | Out-Null
if (-not $vm.WaitForExit(600 * 1000)) {
    Stop-OvmmLabVm
    throw 'The guest did not shut down within 10 minutes.'
}
Stop-OvmmLabVm
Remove-Item -Path $installIso
Set-ItemProperty -Path $disk -Name IsReadOnly -Value $true

Write-Host "Created the clean disk $disk. Run .\Invoke-OvmmShvTest.ps1 next."
