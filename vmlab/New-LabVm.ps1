<#
.SYNOPSIS
    Creates the VMware Workstation VM used to exercise SimpleVisor.

.DESCRIPTION
    Remasters the given Windows install ISO into an unattended one, creates a
    UEFI VM with virtualized VT-x/EPT, installs Windows, configures it for
    loading a test-signed hypervisor, and takes a snapshot that
    Invoke-ShvTest.ps1 reverts to before every run.

    The whole process is unattended and takes roughly 20-40 minutes.

.PARAMETER IsoPath
    A 64-bit Windows 10, Windows 11 or Windows Server ISO. Evaluation media is
    easiest, as it needs no product key.

.PARAMETER ImageIndex
    The index of the edition in install.wim (or install.esd) to install. Use
    -ListImages to see them.

.PARAMETER ImageName
    The name of the edition to install, as an alternative to -ImageIndex, e.g.
    "Windows Server 2025 SERVERSTANDARD" (with the Desktop Experience).

.PARAMETER GuestOs
    The VMware guest OS type. By default, it is picked from the edition's
    name: windows2022srvNext-64 (Windows Server 2025) for Server editions, and
    windows11-64 otherwise.

.PARAMETER SkipHostCheck
    Create the VM even though a hypervisor runs on the host. VMware then cannot
    virtualize VT-x, so the VM is created without it: Windows, the kernel
    debugger and the test deployment all work, but SimpleVisor cannot load.
    The OpenVMM lab (openvmm\) can nest VT-x on such hosts.

.PARAMETER ProductKey
    The product key, if the media needs one to pick an edition. A generic
    installation key, which does not activate Windows, is enough.

.PARAMETER ListImages
    List the editions on the ISO and exit.

.EXAMPLE
    .\New-LabVm.ps1 -IsoPath D:\iso\Server2025_Eval.iso -ImageName 'Windows Server 2025 SERVERSTANDARD'

.EXAMPLE
    .\New-LabVm.ps1 -IsoPath D:\iso\Win11_Enterprise_Eval.iso
#>
[CmdletBinding()]
param (
    [Parameter(Mandatory)] [string] $IsoPath,
    [int] $ImageIndex = 1,
    [string] $ImageName,
    [string] $ProductKey,
    [switch] $ListImages,
    [string] $VmName,
    [string] $VmRoot,
    [string] $GuestOs,
    [int] $CpuCount = 4,
    [int] $MemoryMB = 4096,
    [int] $DiskGB = 64,
    [string] $Locale = 'en-US',
    [string] $OscdimgPath,
    [int] $InstallTimeoutMin = 120,
    [switch] $Gui,
    [switch] $SkipHostCheck,
    [switch] $Force
)

. (Join-Path $PSScriptRoot 'LabCommon.ps1')

if (-not $VmName) { $VmName = $LabDefaults.VmName }
if (-not $VmRoot) { $VmRoot = $LabDefaults.VmRoot }
$IsoPath = (Resolve-Path $IsoPath).Path

function Write-Vmx {
    param (
        [string] $Path,
        [string] $DiskFile,
        [string] $InstallIso
    )

    #
    # Notable settings:
    #   vhv.enable              virtualize VT-x/EPT, so SimpleVisor can load
    #   firmware, secureBoot    UEFI without Secure Boot, so test signing works
    #   serial0                 COM1 on a named pipe, for the kernel debugger.
    #                           The VM is the pipe server, and the debugger on
    #                           the host the client. Yielding the vCPU while
    #                           the guest polls the port keeps KD from spinning.
    #   pciBridge*              the PCIe root ports that the NVMe disk and the
    #                           e1000e NIC plug into
    #   RemoteDisplay.vnc       the console over VNC, on the loopback interface
    #                           only, for Save-LabScreenshot
    #
    $toolsIso = Join-Path (Get-VmwareDir) 'windows.iso'
    $vhv = if ($NoNestedVirtualization) { 'FALSE' } else { 'TRUE' }
    $vmx = [ordered] @{
        '.encoding'                    = 'UTF-8'
        'config.version'               = '8'
        'virtualHW.version'            = '20'
        'displayName'                  = $VmName
        'guestOS'                      = $GuestOs
        'firmware'                     = 'efi'
        'uefi.secureBoot.enabled'      = 'FALSE'
        'numvcpus'                     = [string] $CpuCount
        'cpuid.coresPerSocket'         = [string] $CpuCount
        'memsize'                      = [string] $MemoryMB
        'vhv.enable'                   = $vhv
        'pciBridge0.present'           = 'TRUE'
        'pciBridge4.present'           = 'TRUE'
        'pciBridge4.virtualDev'        = 'pcieRootPort'
        'pciBridge4.functions'         = '8'
        'pciBridge5.present'           = 'TRUE'
        'pciBridge5.virtualDev'        = 'pcieRootPort'
        'pciBridge5.functions'         = '8'
        'pciBridge6.present'           = 'TRUE'
        'pciBridge6.virtualDev'        = 'pcieRootPort'
        'pciBridge6.functions'         = '8'
        'pciBridge7.present'           = 'TRUE'
        'pciBridge7.virtualDev'        = 'pcieRootPort'
        'pciBridge7.functions'         = '8'
        'vmci0.present'                = 'TRUE'
        'hpet0.present'                = 'TRUE'
        'nvme0.present'                = 'TRUE'
        'nvme0:0.present'              = 'TRUE'
        'nvme0:0.fileName'             = $DiskFile
        'sata0.present'                = 'TRUE'
        'sata0:0.present'              = 'TRUE'
        'sata0:0.deviceType'           = 'cdrom-image'
        'sata0:0.fileName'             = $InstallIso
        'sata0:1.present'              = 'TRUE'
        'sata0:1.deviceType'           = 'cdrom-image'
        'sata0:1.fileName'             = $toolsIso
        'ethernet0.present'            = 'TRUE'
        'ethernet0.virtualDev'         = 'e1000e'
        'ethernet0.connectionType'     = 'nat'
        'ethernet0.addressType'        = 'generated'
        'usb.present'                  = 'TRUE'
        'ehci.present'                 = 'TRUE'
        'usb_xhci.present'             = 'TRUE'
        'serial0.present'              = 'TRUE'
        'serial0.startConnected'       = 'TRUE'
        'serial0.fileType'             = 'pipe'
        'serial0.fileName'             = "\\.\pipe\$($LabDefaults.PipeName)"
        'serial0.pipe.endPoint'        = 'server'
        'serial0.tryNoRxLoss'          = 'TRUE'
        'serial0.yieldOnMsrRead'       = 'TRUE'
        'RemoteDisplay.vnc.enabled'    = 'TRUE'
        'RemoteDisplay.vnc.ip'         = '127.0.0.1'
        'RemoteDisplay.vnc.port'       = [string] $LabDefaults.VncPort
        'tools.syncTime'               = 'TRUE'
        'floppy0.present'              = 'FALSE'
    }
    $lines = $vmx.GetEnumerator() | ForEach-Object { "$($_.Key) = `"$($_.Value)`"" }
    [System.IO.File]::WriteAllLines($Path, [string[]] $lines)
}

if ($ListImages) {
    Get-IsoImage -Path $IsoPath | Format-Table ImageIndex, ImageName -AutoSize
    return
}

$image = Resolve-LabImage -IsoPath $IsoPath -ImageIndex $ImageIndex -ImageName $ImageName
if (-not $GuestOs) {
    $GuestOs = if ($image.ImageName -match 'Server') { 'windows2022srvNext-64' } else { 'windows11-64' }
}
Write-Host "Installing '$($image.ImageName)' (index $($image.ImageIndex)) as guest OS type $GuestOs."

$NoNestedVirtualization = $false
if (-not (Test-HostNestedVirtualization)) {
    if (-not $SkipHostCheck) {
        throw 'The host cannot run nested VT-x guests. Fix the above, or pass -SkipHostCheck to create the VM anyway.'
    }

    #
    # VMware refuses to power on a VM that asks for virtualized VT-x on such a
    # host, so leave it out. Recreate the VM once the host is fixed.
    #
    Write-Warning 'Creating the VM without virtualized VT-x/EPT; SimpleVisor will not be able to load in it.'
    $NoNestedVirtualization = $true
}

$vmDir = Join-Path $VmRoot $VmName
$vmxPath = Get-LabVmx -VmName $VmName -VmRoot $VmRoot
if (Test-Path $vmDir) {
    if (-not $Force) {
        throw "$vmDir already exists. Pass -Force to delete and recreate the VM."
    }
    if (Test-Path $vmxPath) {
        if (Test-LabVmRunning -Vmx $vmxPath) {
            Invoke-VmRun -Arguments @('stop', $vmxPath, 'hard') | Out-Null
        }
        Invoke-VmRun -Arguments @('deleteVM', $vmxPath) -AllowFailure | Out-Null
    }
    Remove-Item -Path $vmDir -Recurse -Force
}
New-Item -ItemType Directory -Path $vmDir | Out-Null

$installIso = Join-Path $vmDir "$VmName-install.iso"
New-UnattendedIso -SourceIso $IsoPath -DestinationIso $installIso -ImageIndex $image.ImageIndex `
                  -ComputerName $VmName -ProductKey $ProductKey -Locale $Locale -OscdimgPath $OscdimgPath

$diskFile = "$VmName.vmdk"
Write-Host 'Creating the virtual disk...'
Invoke-NativeCommand -FilePath (Get-VmwareTool 'vmware-vdiskmanager.exe') `
                     -Arguments @('-c', '-s', "${DiskGB}GB", '-a', 'lsilogic', '-t', '0', (Join-Path $vmDir $diskFile)) | Out-Null

Write-Vmx -Path $vmxPath -DiskFile $diskFile -InstallIso $installIso

Write-Host "Starting $VmName and installing Windows; this takes a while..."
$display = if ($Gui) { 'gui' } else { 'nogui' }
Invoke-VmRun -Arguments @('start', $vmxPath, $display) | Out-Null

try {
    Wait-LabGuest -Vmx $vmxPath -TimeoutSec ($InstallTimeoutMin * 60)
}
catch {
    $screenshot = Join-Path $LabDefaults.OutDir 'install-screen.png'
    New-Item -ItemType Directory -Path $LabDefaults.OutDir -Force | Out-Null
    try { Save-LabScreenshot -Path $screenshot } catch { $screenshot = "<none: $_>" }
    throw "$_ The VM is left running; see $screenshot for its console, and C:\vmlab\firstlogon.log in the guest."
}

#
# Shut down cleanly, detach the install media, which is not needed anymore, and
# take the snapshot every test run starts from.
#
Write-Host 'Windows is installed and configured; taking the snapshot...'
Invoke-VmRun -Arguments @('stop', $vmxPath, 'soft') -TimeoutSec 600 | Out-Null
$vmxContent = Get-Content -Path $vmxPath | ForEach-Object {
    if ($_ -like 'sata0:0.*') { $null } else { $_ }
}
[System.IO.File]::WriteAllLines($vmxPath, [string[]] $vmxContent)
Remove-Item -Path $installIso
Invoke-VmRun -Arguments @('snapshot', $vmxPath, $LabDefaults.Snapshot) | Out-Null

Write-Host "Created $vmxPath with snapshot '$($LabDefaults.Snapshot)'. Run .\Invoke-ShvTest.ps1 next."
