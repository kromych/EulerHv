#
# Settings and helpers for the Hyper-V flavor of the SimpleVisor lab, on top of
# ..\LabCommon.ps1. This file is dot-sourced by the scripts next to it, and is
# not meant to be run on its own.
#
# Hyper-V exposes VT-x to a guest with nested virtualization
# (ExposeVirtualizationExtensions), so SimpleVisor runs under the real Hyper-V,
# with VMBus, and the guest sees it underneath ("Hv#1"). The host drives the
# guest through PowerShell Direct, over VMBus, which needs no network and no
# setup in the guest besides the lab credentials.
#

. (Join-Path (Split-Path $PSScriptRoot -Parent) 'LabCommon.ps1')

$script:HvDefaults = @{
    VmName   = 'SimpleVisorLabHv'
    LabDir   = Join-Path $LabDefaults.VmRoot 'SimpleVisorLabHv'
    PipeName = 'SimpleVisorLabHv'
    CpuCount = 4
    MemoryMB = 4096
}

function Get-HvLabFile {
    param ([Parameter(Mandatory)] [string] $Name)

    Join-Path $HvDefaults.LabDir $Name
}

function Assert-HvPrerequisite {
    #
    # The Hyper-V module talks to the Virtual Machine Management service, which
    # members of the Hyper-V Administrators group can use without elevation
    #
    if (-not (Get-Command -Name Get-VM -Module Hyper-V -ErrorAction SilentlyContinue)) {
        throw 'The Hyper-V PowerShell module was not found. Turn on the Hyper-V feature, with its management tools.'
    }
    if (-not (Get-Service -Name vmms -ErrorAction SilentlyContinue | Where-Object Status -eq 'Running')) {
        throw 'The Hyper-V Virtual Machine Management service (vmms) does not run. Turn on Hyper-V and reboot.'
    }
    try {
        Get-VM -ErrorAction Stop | Out-Null
    }
    catch {
        throw "Hyper-V can't be managed from this prompt: $_ Add yourself to the Hyper-V Administrators group, or run elevated."
    }
}

function Get-HvLabVm {
    Get-VM -Name $HvDefaults.VmName -ErrorAction SilentlyContinue
}

function Get-HvLabCredential {
    $password = ConvertTo-SecureString -String $LabDefaults.AdminPassword -AsPlainText -Force
    New-Object System.Management.Automation.PSCredential ($LabDefaults.AdminUser, $password)
}

function Stop-HvLabVm {
    #
    # Turn the VM off, like pulling the plug: it is reverted to its checkpoint
    # before the next use anyway
    #
    $vm = Get-HvLabVm
    if ($vm -and ($vm.State -ne 'Off')) {
        Stop-VM -VM $vm -TurnOff -Force
    }
}

function Invoke-HvGuest {
    param (
        [Parameter(Mandatory)] [scriptblock] $ScriptBlock,
        [object[]] $ArgumentList = @(),
        [int] $TimeoutSec = 120,
        [switch] $AllowFailure
    )

    #
    # Run a script block in the guest over PowerShell Direct, elevated, as the
    # guest's built-in Administrator, and give up after a timeout. The latter
    # matters, as PowerShell Direct waits forever for a guest that SimpleVisor
    # halted or that bugchecked.
    #
    # A timed-out job is left alone rather than stopped: stopping it waits for
    # the guest, which isn't going to answer. It goes away with this process.
    #
    $job = Invoke-Command -VMName $HvDefaults.VmName -Credential (Get-HvLabCredential) -AsJob `
                          -ScriptBlock $ScriptBlock -ArgumentList $ArgumentList
    if (-not (Wait-Job -Job $job -Timeout $TimeoutSec)) {
        $result = [pscustomobject] @{ Output = @(); Error = ''; Failed = $true; TimedOut = $true }
    }
    else {
        $errors = $null
        $output = @(Receive-Job -Job $job -ErrorAction SilentlyContinue -ErrorVariable errors)
        $result = [pscustomobject] @{
            Output   = $output
            Error    = ($errors | ForEach-Object { "$_" }) -join "`n"
            Failed   = ($job.State -ne 'Completed') -or [bool] $errors
            TimedOut = $false
        }
        Remove-Job -Job $job -Force
    }

    if ((-not $AllowFailure) -and $result.Failed) {
        $what = if ($result.TimedOut) { "timed out after $TimeoutSec seconds" } else { "failed: $($result.Error)" }
        throw "Running {$ScriptBlock} in the guest $what"
    }
    $result
}

function Copy-HvToGuest {
    param (
        [Parameter(Mandatory)] [string[]] $Path,
        [Parameter(Mandatory)] [string] $Destination
    )

    #
    # Copy files into an existing directory in the guest, over PowerShell
    # Direct, which needs no Guest Service Interface, unlike Copy-VMFile
    #
    $session = New-PSSession -VMName $HvDefaults.VmName -Credential (Get-HvLabCredential)
    try {
        Copy-Item -ToSession $session -Path $Path -Destination $Destination -Force
    }
    finally {
        Remove-PSSession -Session $session
    }
}

function Copy-HvFromGuest {
    param (
        [Parameter(Mandatory)] [string] $Path,
        [Parameter(Mandatory)] [string] $Destination
    )

    $session = New-PSSession -VMName $HvDefaults.VmName -Credential (Get-HvLabCredential)
    try {
        Copy-Item -FromSession $session -Path $Path -Destination $Destination -Force
    }
    finally {
        Remove-PSSession -Session $session
    }
}

function Test-HvGuestFile {
    param ([Parameter(Mandatory)] [string] $Path)

    #
    # Until the guest has booted and logged on, PowerShell Direct fails, or
    # waits, which counts as the file not being there yet
    #
    $result = Invoke-HvGuest -ScriptBlock { param ($Path) Test-Path -Path $Path } -ArgumentList $Path `
                             -TimeoutSec 60 -AllowFailure
    (-not $result.Failed) -and ($result.Output -contains $true)
}

function Wait-HvGuest {
    param (
        [string] $MarkerFile = "$GuestLabDir\ready.txt",
        [int] $TimeoutSec = 900
    )

    #
    # Wait until PowerShell Direct into the guest works and the given marker
    # file exists, giving up early if the VM stops
    #
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $vm = Get-HvLabVm
        if ((-not $vm) -or ($vm.State -ne 'Running')) {
            throw "The VM $($HvDefaults.VmName) stopped (state: $(if ($vm) { $vm.State } else { 'gone' }))."
        }
        if (Test-HvGuestFile -Path $MarkerFile) {
            return
        }
        Start-Sleep -Seconds 15
    }
    throw "The guest did not become ready within $TimeoutSec seconds (waiting for $MarkerFile)."
}

function Test-HvKernelDebugger {
    param (
        [int] $TimeoutSec = 120,
        [switch] $KeepAttached
    )

    Test-LabKernelDebugger -PipeName $HvDefaults.PipeName -TimeoutSec $TimeoutSec `
                           -TestGuest { Test-HvGuestFile -Path "$GuestLabDir\ready.txt" } `
                           -KeepAttached:$KeepAttached -LogPath (Get-HvLabFile 'kd-attached.log')
}

function Get-HvVmCim {
    $vm = Get-CimInstance -Namespace root\virtualization\v2 -ClassName Msvm_ComputerSystem `
                          -Filter "ElementName = '$($HvDefaults.VmName)'"
    if (-not $vm) {
        throw "The VM $($HvDefaults.VmName) does not exist."
    }
    $vm
}

function Save-HvScreenshot {
    param ([Parameter(Mandatory)] [string] $Path)

    #
    # Save the VM console as a PNG, through Hyper-V's thumbnail image of it.
    # This works whatever the guest's state, e.g. after it hung or bugchecked.
    # The image comes in RGB565, at whatever size is asked for, so ask for the
    # resolution that the guest set.
    #
    Add-Type -AssemblyName System.Drawing
    $vm = Get-HvVmCim
    $head = Get-CimInstance -Namespace root\virtualization\v2 -ClassName Msvm_VideoHead -Filter "SystemName = '$($vm.Name)'" |
            Select-Object -First 1
    $width = if ($head -and $head.CurrentHorizontalResolution) { [int] $head.CurrentHorizontalResolution } else { 1024 }
    $height = if ($head -and $head.CurrentVerticalResolution) { [int] $head.CurrentVerticalResolution } else { 768 }
    $settings = Get-CimAssociatedInstance -InputObject $vm -ResultClassName Msvm_VirtualSystemSettingData |
                Where-Object VirtualSystemType -eq 'Microsoft:Hyper-V:System:Realized'
    $service = Get-CimInstance -Namespace root\virtualization\v2 -ClassName Msvm_VirtualSystemManagementService
    $image = Invoke-CimMethod -InputObject $service -MethodName GetVirtualSystemThumbnailImage -Arguments @{
        TargetSystem = $settings; WidthPixels = [uint16] $width; HeightPixels = [uint16] $height
    }
    if ($image.ReturnValue -ne 0) {
        throw "Hyper-V could not take a thumbnail of the VM (error $($image.ReturnValue))."
    }

    $bitmap = New-Object System.Drawing.Bitmap $width, $height, ([System.Drawing.Imaging.PixelFormat]::Format16bppRgb565)
    try {
        $area = New-Object System.Drawing.Rectangle 0, 0, $width, $height
        $data = $bitmap.LockBits($area, [System.Drawing.Imaging.ImageLockMode]::WriteOnly, $bitmap.PixelFormat)
        try {
            for ($row = 0; $row -lt $height; $row++) {
                [System.Runtime.InteropServices.Marshal]::Copy($image.ImageData, $row * $width * 2,
                    [IntPtr] ($data.Scan0.ToInt64() + $row * $data.Stride), $width * 2)
            }
        }
        finally {
            $bitmap.UnlockBits($data)
        }
        $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    }
    finally {
        $bitmap.Dispose()
    }
}

function Send-HvKeys {
    param (
        [string] $Text = '',
        [switch] $Enter
    )

    #
    # Type into the VM console, through Hyper-V's virtual keyboard, to reach
    # the guest when PowerShell Direct doesn't work. The counterpart of
    # Send-LabKeys, e.g. at a command prompt: Send-HvKeys -Text 'dir' -Enter
    #
    $vm = Get-HvVmCim
    $keyboard = Get-CimAssociatedInstance -InputObject $vm -ResultClassName Msvm_Keyboard
    if ($Text) {
        Invoke-CimMethod -InputObject $keyboard -MethodName TypeText -Arguments @{ AsciiText = $Text } | Out-Null
    }
    if ($Enter) {
        Invoke-CimMethod -InputObject $keyboard -MethodName TypeKey -Arguments @{ KeyCode = [uint32] 0x0D } | Out-Null
    }
}
