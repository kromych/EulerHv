<#
.SYNOPSIS
    Loads and unloads SimpleVisor in the Hyper-V lab VM, and checks what the
    guest sees; the counterpart of ..\Invoke-ShvTest.ps1.

.DESCRIPTION
    Reverts the VM to its clean checkpoint, which is a cold boot, as a VM with
    nested virtualization can't be saved. Then it checks that kd can break in
    over COM1, copies the signed driver and the probe in over PowerShell
    Direct, runs guest\Test-ShvGuest.ps1, and checks the results, like
    Invoke-ShvTest.ps1 does.

    The guest sees Hyper-V's hypervisor leaves ("Hv#1"), so this exercises
    SimpleVisor's paths for running nested under Hyper-V, on the real thing.

    If the guest stops responding, the VM is left running and a screenshot is
    saved, so that the kernel debugger can be attached with
    ..\Connect-LabDebugger.ps1 -HyperV.

.PARAMETER SkipBuild
    Deploy the existing files in vmlab\out instead of building them first.

.PARAMETER SkipDebuggerCheck
    Do not check that the kernel debugger can attach over the VM's serial pipe
    before loading SimpleVisor.

.PARAMETER AttachDebugger
    Keep kd attached over the serial pipe for the whole test, in its own
    window, so that it catches SimpleVisor's output and any halt. Its output
    is shown after the test and logged to kd-attached.log in the lab
    directory. If the guest stops responding, kd stays attached.

.PARAMETER KeepRunning
    Leave the VM running after the test, e.g. to explore it with vmconnect or
    Enter-PSSession -VMName.
#>
[CmdletBinding()]
param (
    [switch] $SkipBuild,
    [switch] $SkipDebuggerCheck,
    [switch] $AttachDebugger,
    [switch] $KeepRunning,
    [int] $BootTimeoutSec = 600,
    [int] $TimeoutSec = 300
)

. (Join-Path $PSScriptRoot 'HvLab.ps1')

Assert-HvPrerequisite
$outDir = $LabDefaults.OutDir
$vm = Get-HvLabVm
if ((-not $vm) -or -not (Get-VMCheckpoint -VM $vm -Name $LabDefaults.Snapshot -ErrorAction SilentlyContinue)) {
    throw "The lab VM $($HvDefaults.VmName), with its checkpoint '$($LabDefaults.Snapshot)', does not exist. Create it with New-HvLabVm.ps1 first."
}

if (-not $SkipBuild) {
    & (Join-Path (Split-Path $PSScriptRoot -Parent) 'Build-LabArtifacts.ps1')
}

#
# Start from the clean checkpoint every time, as a failed run can leave the
# guest in any state
#
Write-Host "Reverting $($vm.Name) to checkpoint '$($LabDefaults.Snapshot)' and starting it..."
Stop-HvLabVm
Restore-VMCheckpoint -VM $vm -Name $LabDefaults.Snapshot -Confirm:$false
Start-VM -VM $vm
Wait-HvGuest -TimeoutSec $BootTimeoutSec

$kdCheck = $null
if ($AttachDebugger -or -not $SkipDebuggerCheck) {
    Write-Host 'Checking that the kernel debugger can break in over the serial pipe...'
    $kdCheck = Test-HvKernelDebugger -KeepAttached:$AttachDebugger
    if ($kdCheck.Connected -and $kdCheck.Resumed) {
        Write-Host "  connected: $($kdCheck.Target)"
    }
    else {
        Write-Warning 'kd could not break into the guest; see the kd log below.'
        Write-Host $kdCheck.Log
    }
}

Write-Host 'Copying the test files into the guest...'
$files = @(
    (Join-Path $outDir 'shv.sys'),
    (Join-Path $outDir 'shvcheck.exe'),
    (Join-Path $outDir 'SimpleVisorLab.cer'),
    (Join-Path (Split-Path $PSScriptRoot -Parent) 'guest\Test-ShvGuest.ps1')
)
foreach ($file in $files) {
    if (-not (Test-Path $file)) {
        throw "$file is missing. Run Build-LabArtifacts.ps1, or drop -SkipBuild."
    }
}
Copy-HvToGuest -Path $files -Destination $GuestLabDir

Write-Host 'Loading and unloading SimpleVisor in the guest...'
$run = Invoke-HvGuest -TimeoutSec $TimeoutSec -AllowFailure -ArgumentList "$GuestLabDir\Test-ShvGuest.ps1" -ScriptBlock {
    param ($Script)
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Script
}
if ($run.TimedOut) {
    $screenshot = Get-HvLabFile 'guest-screen.png'
    try {
        Save-HvScreenshot -Path $screenshot
    }
    catch {
        $screenshot = "<none: $_>"
    }
    Write-Host ''
    Write-Host "FAIL: the guest stopped responding within $TimeoutSec seconds." -ForegroundColor Red
    Write-Host "  The VM is left running. Screenshot of its console: $screenshot"
    if ($kdCheck.Process) {
        Write-Host "  kd is attached (PID $($kdCheck.Process.Id)), logging to $($kdCheck.LogPath)."
    }
    else {
        Write-Host '  Attach the kernel debugger with ..\Connect-LabDebugger.ps1 -HyperV to see where it stopped.'
    }
    exit 2
}

$resultFile = Get-HvLabFile 'result.txt'
Remove-Item -Path $resultFile -ErrorAction SilentlyContinue
Copy-HvFromGuest -Path "$GuestLabDir\result.txt" -Destination $resultFile

$failed = Show-LabResult -Result (Read-LabResult -Path $resultFile) -ResultFile $resultFile -KdCheck $kdCheck `
                         -NoVmxHint 'Hyper-V was expected to expose it (ExposeVirtualizationExtensions); see Get-VMProcessor.'

if ($kdCheck.Process) {
    Show-LabDebuggerOutput -KdCheck $kdCheck
}

if (-not $KeepRunning) {
    Stop-LabKernelDebugger -KdCheck $kdCheck
    Stop-HvLabVm
}

if ($failed -ne 0) {
    exit 1
}
