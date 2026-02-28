<#
.SYNOPSIS
    Attaches the kernel debugger to the lab VM.

.DESCRIPTION
    The lab VM has kernel debugging enabled on COM1, which VMware connects to a
    named pipe on the host. This starts WinDbg (or kd, with -Console) from the
    Enterprise WDK on that pipe, with symbols for the lab build of SimpleVisor.

    SimpleVisor breaks into the debugger when it hits a state it cannot
    recover from (see ShvVmxHalt), after saving the details of the failing
    VM-Exit in its per-processor data (the Fatal* fields of SHV_VP_DATA).

    The VM must be running. Attaching works at any time, including after it
    has already stopped responding.

.PARAMETER Console
    Use kd in this console instead of WinDbg.

.PARAMETER Verify
    Only check that the debugger can break in over the pipe, and then resume
    the guest and detach. Invoke-ShvTest.ps1 runs this before every test.
#>
[CmdletBinding()]
param (
    [string] $WdkRoot,
    [switch] $Console,
    [switch] $Verify
)

. (Join-Path $PSScriptRoot 'LabCommon.ps1')

if (-not $WdkRoot) { $WdkRoot = $LabDefaults.WdkRoot }

if ($Verify) {
    $result = Test-LabKernelDebugger -WdkRoot $WdkRoot
    if (-not ($result.Connected -and $result.Resumed)) {
        Write-Host $result.Log
        throw 'kd could not break into the lab VM over its serial pipe, and resume it.'
    }
    Write-Host "Kernel debugger connected and resumed the guest: $($result.Target)"
    return
}

$debugger = Get-LabDebugger -WdkRoot $WdkRoot -Console:$Console
$arguments = Get-LabDebuggerArguments
if ($Console) {
    & $debugger @arguments
}
else {
    Start-Process -FilePath $debugger -ArgumentList (($arguments | ForEach-Object { ConvertTo-CommandLineArgument $_ }) -join ' ')
}
