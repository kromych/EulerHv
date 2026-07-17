<#
.SYNOPSIS
    Loads and unloads SimpleVisor in the OpenVMM lab VM, and checks what the
    guest sees; the counterpart of ..\Invoke-ShvTest.ps1.

.DESCRIPTION
    Boots a throwaway in-memory diff of the clean disk, the equivalent of
    reverting to a snapshot, with VT-x exposed to the guest. Then it checks
    that kd can break in over COM1, copies the signed driver and the probe in
    over SSH, runs guest\Test-ShvGuest.ps1, and checks the results, like
    Invoke-ShvTest.ps1 does.

    Under OpenVMM, the guest also sees Hyper-V's hypervisor leaves ("Hv#1"),
    so this exercises SimpleVisor's paths for running nested under Hyper-V.

    If the guest stops responding, OpenVMM is left running, so that the kernel
    debugger can be attached with ..\Connect-LabDebugger.ps1 -OpenVmm. Note
    that the console is black in this configuration, which has no synthetic
    video, so screenshots don't help here.

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
    Leave the VM running after the test, e.g. to explore it over SSH. Its disk
    is a throwaway diff, so changes are lost when it stops.
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

. (Join-Path $PSScriptRoot 'OvmmLab.ps1')

$outDir = $LabDefaults.OutDir
if (-not (Test-Path (Get-OvmmLabFile 'disk.img'))) {
    throw "The lab VM does not exist in $($OvmmDefaults.LabDir). Create it with New-OvmmLabVm.ps1 first."
}

if (-not $SkipBuild) {
    & (Join-Path (Split-Path $PSScriptRoot -Parent) 'Build-LabArtifacts.ps1')
}

Write-Host 'Booting a fresh diff of the clean disk, with VT-x exposed...'
Stop-OvmmLabVm
$vm = Start-OvmmLabVm -Mode test
Wait-OvmmGuest -Process $vm -TimeoutSec $BootTimeoutSec

$kdCheck = $null
if ($AttachDebugger -or -not $SkipDebuggerCheck) {
    Write-Host 'Checking that the kernel debugger can break in over the serial pipe...'
    $kdCheck = Test-OvmmKernelDebugger -KeepAttached:$AttachDebugger
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
    (Join-Path $outDir 'euler.sys'),
    (Join-Path $outDir 'eulercheck.exe'),
    (Join-Path $outDir 'SimpleVisorLab.cer'),
    (Join-Path (Split-Path $PSScriptRoot -Parent) 'guest\Test-ShvGuest.ps1')
)
foreach ($file in $files) {
    if (-not (Test-Path $file)) {
        throw "$file is missing. Run Build-LabArtifacts.ps1, or drop -SkipBuild."
    }
    Copy-OvmmToGuest -Path $file -Destination "$GuestLabDir\$(Split-Path $file -Leaf)"
}

Write-Host 'Loading and unloading SimpleVisor in the guest...'
$run = Invoke-OvmmGuest -TimeoutSec $TimeoutSec -AllowFailure `
                        -Command "powershell -NoProfile -ExecutionPolicy Bypass -File $GuestLabDir\Test-ShvGuest.ps1"
if ($run.TimedOut) {
    Write-Host ''
    Write-Host "FAIL: the guest stopped responding within $TimeoutSec seconds." -ForegroundColor Red
    Write-Host "  OpenVMM (PID $($vm.Id)) is left running; stop it with Stop-OvmmLabVm (dot-source OvmmLab.ps1)."
    if ($kdCheck.Process) {
        Write-Host "  kd is attached (PID $($kdCheck.Process.Id)), logging to $($kdCheck.LogPath)."
    }
    else {
        Write-Host '  Attach the kernel debugger with ..\Connect-LabDebugger.ps1 -OpenVmm to see where it stopped.'
    }
    exit 2
}

$resultFile = Get-OvmmLabFile 'result.txt'
Remove-Item -Path $resultFile -ErrorAction SilentlyContinue
Copy-OvmmFromGuest -Path "$GuestLabDir\result.txt" -Destination $resultFile

$failed = Show-LabResult -Result (Read-LabResult -Path $resultFile) -ResultFile $resultFile -KdCheck $kdCheck `
                         -NoVmxHint 'OpenVMM was expected to expose it (--nested-virt); see the openvmm-test.log in the lab directory.'

if ($kdCheck.Process) {
    Show-LabDebuggerOutput -KdCheck $kdCheck
}

if (-not $KeepRunning) {
    Stop-LabKernelDebugger -KdCheck $kdCheck
    Stop-OvmmLabVm
}

if ($failed -ne 0) {
    exit 1
}
