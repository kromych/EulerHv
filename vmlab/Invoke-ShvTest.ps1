<#
.SYNOPSIS
    Loads and unloads SimpleVisor in the lab VM, and checks what the guest sees.

.DESCRIPTION
    Reverts the lab VM to its clean snapshot, copies in the signed driver and
    the shvcheck.exe probe, and runs guest\Test-ShvGuest.ps1 in the guest. The
    probe runs before SimpleVisor is loaded, while it is loaded, and after it
    is unloaded, and this script then checks that:

      - SimpleVisor loads and unloads, and is detected only while loaded
      - the hypervisor vendor (40000000h) and interface (40000001h) leaves
        still describe VMware while SimpleVisor is loaded
      - WAITPKG is either hidden, or UMONITOR/UMWAIT/TPAUSE actually work

    Before loading SimpleVisor, it also checks that the kernel debugger is
    enabled on COM1 in the guest, and that kd can break in over the serial
    pipe, so that it is known to be available if SimpleVisor halts.

    If the guest stops responding (SimpleVisor halted, or the guest
    bugchecked), the VM is left running and a screenshot is saved, so that the
    kernel debugger can be attached with Connect-LabDebugger.ps1.

.PARAMETER SkipBuild
    Deploy the existing files in vmlab\out instead of building them first.

.PARAMETER SkipDebuggerCheck
    Do not check that the kernel debugger can attach over the VM's serial pipe
    before loading SimpleVisor.

.PARAMETER KeepRunning
    Leave the VM running after the test, e.g. to explore it.
#>
[CmdletBinding()]
param (
    [string] $VmName,
    [string] $VmRoot,
    [switch] $SkipBuild,
    [switch] $KeepRunning,
    [switch] $Gui,
    [switch] $SkipDebuggerCheck,
    [int] $TimeoutSec = 300
)

. (Join-Path $PSScriptRoot 'LabCommon.ps1')

if (-not $VmName) { $VmName = $LabDefaults.VmName }
if (-not $VmRoot) { $VmRoot = $LabDefaults.VmRoot }
$vmxPath = Get-LabVmx -VmName $VmName -VmRoot $VmRoot
$outDir = $LabDefaults.OutDir
if (-not (Test-Path $vmxPath)) {
    throw "The lab VM $vmxPath does not exist. Create it with New-LabVm.ps1 first."
}

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'Build-LabArtifacts.ps1')
}

#
# Start from the clean snapshot every time, as a failed run can leave the guest
# in any state
#
Write-Host "Reverting $VmName to snapshot '$($LabDefaults.Snapshot)' and starting it..."
if (Test-LabVmRunning -Vmx $vmxPath) {
    Invoke-VmRun -Arguments @('stop', $vmxPath, 'hard') | Out-Null
}
Invoke-VmRun -Arguments @('revertToSnapshot', $vmxPath, $LabDefaults.Snapshot) | Out-Null
$display = if ($Gui) { 'gui' } else { 'nogui' }
Invoke-VmRun -Arguments @('start', $vmxPath, $display) | Out-Null
Wait-LabGuest -Vmx $vmxPath

$kdCheck = $null
if (-not $SkipDebuggerCheck) {
    Write-Host 'Checking that the kernel debugger can break in over the serial pipe...'
    $kdCheck = Test-LabKernelDebugger -Vmx $vmxPath
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
    (Join-Path $PSScriptRoot 'guest\Test-ShvGuest.ps1')
)
foreach ($file in $files) {
    if (-not (Test-Path $file)) {
        throw "$file is missing. Run Build-LabArtifacts.ps1, or drop -SkipBuild."
    }
    Invoke-VmRun -Guest -Arguments @('copyFileFromHostToGuest', $vmxPath, $file, "$GuestLabDir\$(Split-Path $file -Leaf)") | Out-Null
}

Write-Host 'Loading and unloading SimpleVisor in the guest...'
$run = Invoke-VmRun -Guest -TimeoutSec $TimeoutSec -AllowFailure -Arguments @(
    'runProgramInGuest', $vmxPath,
    'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe',
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "$GuestLabDir\Test-ShvGuest.ps1")

if ($run.TimedOut) {
    $screenshot = Join-Path $outDir 'guest-screen.png'
    try {
        Save-LabScreenshot -Path $screenshot
    }
    catch {
        $screenshot = "<none: $_>"
    }
    Write-Host ''
    Write-Host "FAIL: the guest stopped responding within $TimeoutSec seconds." -ForegroundColor Red
    Write-Host "  The VM is left running. Screenshot of its console: $screenshot"
    Write-Host '  Attach the kernel debugger with .\Connect-LabDebugger.ps1 to see where it stopped.'
    exit 2
}

$resultFile = Join-Path $outDir 'result.txt'
Remove-Item -Path $resultFile -ErrorAction SilentlyContinue
Invoke-VmRun -Guest -Arguments @('copyFileFromGuestToHost', $vmxPath, "$GuestLabDir\result.txt", $resultFile) | Out-Null

$result = @{}
foreach ($line in Get-Content -Path $resultFile) {
    $key, $value = $line -split '=', 2
    $result[$key] = $value
}

function Get-Result {
    param ([string] $Key)

    if ($result.ContainsKey($Key)) { $result[$Key] } else { '<missing>' }
}

#
# Check the results
#
$checks = @(
    @{ Name = 'Guest test ran to completion';                       Pass = (Get-Result 'done') -eq '1' }
    @{ Name = 'Kernel debugging enabled on COM1 at 115200 baud';    Pass = ((Get-Result 'kd.debug') -eq 'Yes') -and ((Get-Result 'kd.debugtype') -eq 'Serial') -and
                                                                           ((Get-Result 'kd.debugport') -eq '1') -and ((Get-Result 'kd.baudrate') -eq '115200') }
    @{ Name = 'SimpleVisor not detected before loading';            Pass = (Get-Result 'before.simplevisor') -eq '0' }
    @{ Name = 'sc start simplevisor succeeded';                     Pass = (Get-Result 'load.exit') -eq '0' }
    @{ Name = 'SimpleVisor detected while loaded';                  Pass = (Get-Result 'loaded.simplevisor') -eq '1' }
    @{ Name = 'Hypervisor Present-bit set while loaded';            Pass = (Get-Result 'loaded.hypervisor_present') -eq '1' }
    @{ Name = 'Hypervisor vendor leaf unchanged while loaded';      Pass = (Get-Result 'loaded.hv_vendor') -eq (Get-Result 'before.hv_vendor') }
    @{ Name = 'Hypervisor interface leaf unchanged while loaded';   Pass = (Get-Result 'loaded.hv_interface') -eq (Get-Result 'before.hv_interface') }
    @{ Name = 'WAITPKG hidden, or usable, while loaded';            Pass = (Get-Result 'loaded.waitpkg_exec') -in @('ok', 'skipped') }
    @{ Name = 'sc stop simplevisor succeeded';                      Pass = (Get-Result 'unload.exit') -eq '0' }
    @{ Name = 'SimpleVisor not detected after unloading';           Pass = (Get-Result 'after.simplevisor') -eq '0' }
)
if ($kdCheck) {
    $checks = @(@{ Name = 'kd broke in over the serial pipe, and resumed'; Pass = $kdCheck.Connected -and $kdCheck.Resumed }) + $checks
}

Write-Host ''
Write-Host ("Underlying hypervisor: {0} ({1}); WAITPKG before/loaded: {2}/{3} ({4})" -f
    (Get-Result 'before.hv_vendor'), (Get-Result 'before.hv_interface'),
    (Get-Result 'before.waitpkg'), (Get-Result 'loaded.waitpkg'), (Get-Result 'loaded.waitpkg_exec'))
if ((Get-Result 'load.exit') -ne '0') {
    Write-Host "sc start: $(Get-Result 'load.output')"
}
if ((Get-Result 'before.vmx') -ne '1') {
    Write-Warning ('The guest has no VMX, so SimpleVisor cannot load in it. The VM was created ' +
                   'without virtualized VT-x/EPT, because the host runs a hypervisor; see README.md.')
}
$failed = 0
foreach ($check in $checks) {
    if ($check.Pass) {
        Write-Host "  PASS  $($check.Name)" -ForegroundColor Green
    }
    else {
        Write-Host "  FAIL  $($check.Name)" -ForegroundColor Red
        $failed++
    }
}
Write-Host "Full results: $resultFile"

if (-not $KeepRunning) {
    Invoke-VmRun -Arguments @('stop', $vmxPath, 'hard') | Out-Null
}

if ($failed -ne 0) {
    exit 1
}
