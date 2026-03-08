<#
.SYNOPSIS
    Attaches the kernel debugger to the lab VM.

.DESCRIPTION
    The lab VM has kernel debugging enabled on COM1, which VMware (or OpenVMM,
    with -OpenVmm, or Hyper-V, with -HyperV) connects to a named pipe on the
    host. This starts WinDbg (or kd, with -Console) from the Enterprise WDK on
    that pipe, with symbols for the lab build of SimpleVisor.

    SimpleVisor breaks into the debugger when it hits a state it cannot
    recover from (see ShvVmxHalt), after saving the details of the failing
    VM-Exit in its per-processor data (the Fatal* fields of SHV_VP_DATA).

    The VM must be running. Attaching works at any time, including after it
    has already stopped responding.

.PARAMETER OpenVmm
    Attach to the OpenVMM lab VM (openvmm\) instead of the VMware one.

.PARAMETER HyperV
    Attach to the Hyper-V lab VM (hyperv\) instead of the VMware one.

.PARAMETER Console
    Use kd in this console instead of WinDbg.

.PARAMETER Verify
    Only check that the debugger can break in over the pipe, and then resume
    the guest and detach. Invoke-ShvTest.ps1 runs this before every test.
#>
[CmdletBinding()]
param (
    [string] $WdkRoot,
    [switch] $OpenVmm,
    [switch] $HyperV,
    [switch] $Console,
    [switch] $Verify
)

if ($OpenVmm) {
    . (Join-Path $PSScriptRoot 'openvmm\OvmmLab.ps1')
    $pipeName = $OvmmDefaults.PipeName
}
elseif ($HyperV) {
    . (Join-Path $PSScriptRoot 'hyperv\HvLab.ps1')
    $pipeName = $HvDefaults.PipeName
}
else {
    . (Join-Path $PSScriptRoot 'LabCommon.ps1')
    $pipeName = $LabDefaults.PipeName
}

if (-not $WdkRoot) { $WdkRoot = $LabDefaults.WdkRoot }

if ($Verify) {
    $result = if ($OpenVmm) { Test-OvmmKernelDebugger }
              elseif ($HyperV) { Test-HvKernelDebugger }
              else { Test-LabKernelDebugger -WdkRoot $WdkRoot }
    if (-not ($result.Connected -and $result.Resumed)) {
        Write-Host $result.Log
        throw 'kd could not break into the lab VM over its serial pipe, and resume it.'
    }
    Write-Host "Kernel debugger connected and resumed the guest: $($result.Target)"
    return
}

$debugger = Get-LabDebugger -WdkRoot $WdkRoot -Console:$Console
$arguments = Get-LabDebuggerArguments -PipeName $pipeName
if ($Console) {
    & $debugger @arguments
}
else {
    Start-Process -FilePath $debugger -ArgumentList (($arguments | ForEach-Object { ConvertTo-CommandLineArgument $_ }) -join ' ')
}
