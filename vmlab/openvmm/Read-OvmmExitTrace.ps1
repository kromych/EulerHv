<#
.SYNOPSIS
    Reads SimpleVisor's exit trace, and its fatal exits, from the memory of a
    hung OpenVMM lab guest.

.DESCRIPTION
    When SimpleVisor halts, every processor stops in root mode, and kd shows
    little of what led there. SimpleVisor keeps the last VM-Exits on all
    processors in a ring, euler!ShvExitTrace, and the details of a fatal exit
    in the Fatal* fields of each processor's SHV_VP_DATA. This reads both
    through OpenVMM's console, with read-memory, walking the guest's page
    tables from the CR3 that OpenVMM reports for a processor in root mode.

    Euler's load address and the VP data come from kd's log of the test, which
    Invoke-OvmmShvTest.ps1 -AttachDebugger writes: the break into Euler and the
    "fatal VM-Exit" lines. The symbols come from the build in vmlab\out. The
    trace prints newest first.

.PARAMETER KdLog
    kd's log of the test. Defaults to kd-attached.log in the lab directory.
#>
[CmdletBinding()]
param (
    [string] $KdLog
)

. (Join-Path $PSScriptRoot 'OvmmLab.ps1')
if (-not $KdLog) { $KdLog = Get-OvmmLabFile 'kd-attached.log' }

function Read-GuestPhysical {
    param ([UInt64] $Address, [int] $Size)

    #
    # read-memory with -f prints nothing, so wait for the file instead
    #
    $file = Get-OvmmLabFile 'read-memory.bin'
    Remove-Item -Path $file -ErrorAction SilentlyContinue
    [void] (Invoke-OvmmConsole -Command ("read-memory 0x{0:x} {1} -f '{2}'" -f $Address, $Size, $file) -TimeoutSec 0)
    for ($i = 0; ($i -lt 80) -and -not ((Test-Path $file) -and ((Get-Item $file).Length -ge $Size)); $i++) {
        Start-Sleep -Milliseconds 50
    }
    if (-not (Test-Path $file)) {
        throw ('read-memory 0x{0:x} failed.' -f $Address)
    }
    [System.IO.File]::ReadAllBytes($file)
}

function Get-GuestPhysicalAddress {
    param ([UInt64] $Cr3, [UInt64] $Address)

    #
    # A 4-level walk, with 1 GB and 2 MB pages
    #
    $mask = [UInt64] 0x000FFFFFFFFFF000
    $table = $Cr3 -band $mask
    foreach ($shift in 39, 30, 21, 12) {
        $index = ($Address -shr $shift) -band 0x1FF
        $entry = [BitConverter]::ToUInt64((Read-GuestPhysical ($table + $index * 8) 8), 0)
        if (($entry -band 1) -eq 0) {
            throw ('0x{0:x} is not mapped (entry 0x{1:x} at level {2}).' -f $Address, $entry, $shift)
        }
        if (($shift -in 30, 21) -and (($entry -band 0x80) -ne 0)) {
            $pageMask = ([UInt64] 1 -shl $shift) - 1
            return ($entry -band $mask -band (-bnot $pageMask)) + ($Address -band $pageMask)
        }
        $table = $entry -band $mask
    }
    $table + ($Address -band 0xFFF)
}

function Read-GuestVirtual {
    param ([UInt64] $Cr3, [UInt64] $Address, [int] $Size)

    $result = New-Object System.Collections.Generic.List[byte]
    while ($Size -gt 0) {
        $chunk = [Math]::Min($Size, 0x1000 - ($Address -band 0xFFF))
        $result.AddRange([byte[]] (Read-GuestPhysical (Get-GuestPhysicalAddress $Cr3 $Address) $chunk))
        $Address += $chunk
        $Size -= $chunk
    }
    , $result.ToArray()
}

#
# Where kd broke into Euler, and the VP data of the fatal exits
#
$kdText = Get-Content -Raw -Path $KdLog
$break = ([regex] 'euler!(\w+)\+0x([0-9a-f]+):\s*\r?\n([0-9a-f]{8})`([0-9a-f]{8})').Match($kdText)
if (-not $break.Success) {
    throw "kd did not break into Euler, according to $KdLog."
}
$vps = @(([regex] 'fatal (?:VM-Exit|#VMEXIT) on CPU \d+, VP data 0x([0-9A-F]+)').Matches($kdText) |
         ForEach-Object { [UInt64] "0x$($_.Groups[1].Value)" } | Sort-Object -Unique)

#
# The symbols, from euler.sys opened as a dump, at its default base
#
$symLog = Join-Path ([System.IO.Path]::GetTempPath()) "shvlab-trace-symbols-$PID.log"
$env:_NT_SYMBOL_PATH = $null
& (Get-LabDebugger -Console) -z (Join-Path $LabDefaults.OutDir 'euler.sys') -y $LabDefaults.OutDir -logo $symLog `
    -c ("lm m euler; x euler!$($break.Groups[1].Value); x euler!ShvExitTrace; x euler!ShvExitTraceSequence; " +
        "dt euler!_SHV_VP_DATA ProcessorIndex FatalExitReason; ?? sizeof(euler!ShvExitTrace); q") | Out-Null
$symText = Get-Content -Raw -Path $symLog
Remove-Item -Path $symLog -ErrorAction SilentlyContinue
$imageBase = [UInt64] ('0x' + (([regex] '([0-9a-f]{8})`([0-9a-f]{8}) [0-9a-f`]+ +euler ').Match($symText).Groups[1..2].Value -join ''))
function Get-Rva {
    param ([string] $Name)

    $m = ([regex] "([0-9a-f]{8})``([0-9a-f]{8}) euler!$Name\b").Match($symText)
    [UInt64] "0x$($m.Groups[1].Value)$($m.Groups[2].Value)" - $imageBase
}
$base = [UInt64] "0x$($break.Groups[3].Value)$($break.Groups[4].Value)" -
        [UInt64] "0x$($break.Groups[2].Value)" - (Get-Rva $break.Groups[1].Value)
$indexOffset = [UInt64] "0x$(([regex] '\+0x([0-9a-f]+) ProcessorIndex').Match($symText).Groups[1].Value)"
$fatalOffset = [UInt64] "0x$(([regex] '\+0x([0-9a-f]+) FatalExitReason').Match($symText).Groups[1].Value)"
$traceSize = [int] [UInt64] "0x$(([regex] 'unsigned int64 0x([0-9a-f]+)').Match($symText).Groups[1].Value)"
$entries = $traceSize / 32
'Euler at 0x{0:x}, broke in at {1}+0x{2}' -f $base, $break.Groups[1].Value, $break.Groups[2].Value

#
# Root mode runs with the system's CR3 on every processor
#
$registers = Invoke-OvmmConsole -Command 'inspect -r -l 6 vm/partition/vp/0' -TimeoutSec 20
$cr3 = [Convert]::ToUInt64((([regex] 'cr3: 0x([0-9a-f]+)').Match($registers).Groups[1].Value), 16)

$sequence = [BitConverter]::ToUInt64((Read-GuestVirtual $cr3 ($base + (Get-Rva 'ShvExitTraceSequence')) 8), 0)
"$sequence VM-Exits traced, the last $([Math]::Min($sequence, $entries)) kept"
foreach ($vp in $vps) {
    $fatal = Read-GuestVirtual $cr3 ($vp + $fatalOffset) 0x30
    $index = [BitConverter]::ToUInt32((Read-GuestVirtual $cr3 ($vp + $indexOffset) 4), 0)
    'CPU {0} (VP data 0x{1:x}): fatal exit 0x{2:x}, qualification 0x{3:x}, GPA 0x{4:x}, GLA 0x{5:x}, RIP 0x{6:x}, RSP 0x{7:x}' -f $index, $vp,
        [BitConverter]::ToUInt64($fatal, 0), [BitConverter]::ToUInt64($fatal, 8), [BitConverter]::ToUInt64($fatal, 16),
        [BitConverter]::ToUInt64($fatal, 24), [BitConverter]::ToUInt64($fatal, 32), [BitConverter]::ToUInt64($fatal, 40)
}

#
# The entries, as SHV_EXIT_TRACE_ENTRY in shvutil.c lays them out. Event
# reasons 0xFF00 and up are SimpleVisor's own, see SHV_TRACE_* in shv.h
#
$trace = Read-GuestVirtual $cr3 ($base + (Get-Rva 'ShvExitTrace')) $traceSize
$rows = for ($i = 0; $i -lt $entries; $i++) {
    $o = $i * 32
    $seq = [BitConverter]::ToUInt32($trace, $o)
    if ($seq -eq 0) {
        continue
    }
    $reason = [BitConverter]::ToUInt16($trace, $o + 6)
    $info = [BitConverter]::ToUInt32($trace, $o + 20)
    $qualification = [BitConverter]::ToUInt64($trace, $o + 24)
    $what = switch ($reason) {
        0x0    { 'NMI or exception' }
        0x12   { 'hypercall 0x{0:x}{1}, input 0x{2:x}' -f ($info -band 0xFFFF), $(if ($info -band 0x10000) { ' (fast)' }), $qualification }
        0x1f   { 'RDMSR 0x{0:x}' -f $info }
        0x20   { 'WRMSR 0x{0:x} = 0x{1:x}' -f $info, $qualification }
        0xFF00 { 'flush request to CPU {0}{1}, requested {2}' -f ($info -band 0xFFFF), $(if ($info -band 0x80000000) { ', NMI sent' }), $qualification }
        0xFF01 { if ($info) { "NMI: flush IPI, requested $qualification" } else { 'NMI: not a flush IPI' } }
        default { '' }
    }
    [pscustomobject] @{
        Seq   = $seq
        Torn  = $seq -ne [BitConverter]::ToUInt32($trace, $o + 16)
        Cpu   = [BitConverter]::ToUInt16($trace, $o + 4)
        Exit  = '0x{0:x}' -f $reason
        Rip   = '0x{0:x}' -f [BitConverter]::ToUInt64($trace, $o + 8)
        Info  = '0x{0:x}' -f $info
        Qual  = '0x{0:x}' -f $qualification
        What  = $what
    }
}
$rows | Sort-Object Seq -Descending | Format-Table -AutoSize | Out-String -Width 250
