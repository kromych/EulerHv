#
# Shared settings and helpers for the SimpleVisor VMware lab scripts. This file
# is dot-sourced by the other scripts, and is not meant to be run on its own.
#

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

#
# Windows PowerShell started from PowerShell 7, even indirectly, inherits its
# module path, and then loads PowerShell 7's own modules, which fail to load.
# That takes ConvertTo-SecureString and the certificate drive's
# -CodeSigningCert with them. Keep to Windows PowerShell's modules there.
#
if ($PSVersionTable.PSEdition -eq 'Desktop') {
    $env:PSModulePath = ($env:PSModulePath -split ';' |
                         Where-Object { $_ -and ($_ -notmatch '\\PowerShell\\(7[^\\]*\\)?Modules\\?$') }) -join ';'
}

$script:RepoRoot = Split-Path $PSScriptRoot -Parent

#
# Lab defaults. Each can be overridden by the matching parameter of the
# scripts, or by the environment variable shown.
#
$script:LabDefaults = @{
    VmName        = 'SimpleVisorLab'
    VmRoot        = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Virtual Machines'
    AdminUser     = 'Administrator'
    AdminPassword = 'SimpleVisor!Lab1'
    PipeName      = 'SimpleVisorLab'
    VncPort       = 5917
    Snapshot      = 'clean'
    OutDir        = Join-Path $PSScriptRoot 'out'
    WdkRoot       = if ($env:SHV_WDK_ROOT) { $env:SHV_WDK_ROOT } else { Join-Path (Split-Path $script:RepoRoot -Parent) 'wdk' }
}

#
# Path, inside the guest, where the lab keeps its files and markers
#
$script:GuestLabDir = 'C:\vmlab'

function Get-LabVmx {
    param (
        [string] $VmName = $LabDefaults.VmName,
        [string] $VmRoot = $LabDefaults.VmRoot
    )

    Join-Path (Join-Path $VmRoot $VmName) "$VmName.vmx"
}

function Get-VmwareDir {
    #
    # Prefer the install path that VMware Workstation records in the registry,
    # and fall back to the default install locations.
    #
    foreach ($key in 'HKLM:\SOFTWARE\WOW6432Node\VMware, Inc.\VMware Workstation',
                     'HKLM:\SOFTWARE\VMware, Inc.\VMware Workstation') {
        $item = Get-ItemProperty -Path $key -Name InstallPath -ErrorAction SilentlyContinue
        if ($item -and (Test-Path (Join-Path $item.InstallPath 'vmrun.exe'))) {
            return $item.InstallPath.TrimEnd('\')
        }
    }

    foreach ($dir in "$env:ProgramFiles\VMware\VMware Workstation",
                     "${env:ProgramFiles(x86)}\VMware\VMware Workstation") {
        if (Test-Path (Join-Path $dir 'vmrun.exe')) {
            return $dir
        }
    }

    throw 'VMware Workstation was not found. Install VMware Workstation Pro 17 or newer.'
}

function Get-VmwareTool {
    param ([Parameter(Mandatory)] [string] $Name)

    $path = Join-Path (Get-VmwareDir) $Name
    if (-not (Test-Path $path)) {
        throw "$Name was not found in the VMware Workstation directory."
    }
    $path
}

function ConvertTo-CommandLineArgument {
    param ([string] $Argument)

    #
    # Quote an argument the way the Microsoft C runtime parses it back, so that
    # paths with spaces survive the trip intact.
    #
    if ($Argument -and ($Argument -notmatch '[\s"]')) {
        return $Argument
    }
    $escaped = $Argument -replace '(\\*)"', '$1$1\"'
    $escaped = $escaped -replace '(\\+)$', '$1$1'
    "`"$escaped`""
}

function Invoke-NativeCommand {
    param (
        [Parameter(Mandatory)] [string] $FilePath,
        [string[]] $Arguments = @(),
        [int] $TimeoutSec = 0,
        [switch] $AllowFailure
    )

    #
    # Run a native tool, capturing its output, and optionally giving up after a
    # timeout. The latter matters for guest operations: if SimpleVisor halts or
    # bugchecks the guest, vmrun would otherwise wait forever.
    #
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $FilePath
    $psi.Arguments = ($Arguments | ForEach-Object { ConvertTo-CommandLineArgument $_ }) -join ' '
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true

    $process = [System.Diagnostics.Process]::Start($psi)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()

    $waitMs = if ($TimeoutSec -gt 0) { $TimeoutSec * 1000 } else { -1 }
    if (-not $process.WaitForExit($waitMs)) {
        $process.Kill()
        $process.WaitForExit()
        $result = [pscustomobject] @{ ExitCode = $null; Output = $stdout.Result + $stderr.Result; TimedOut = $true }
    }
    else {
        $result = [pscustomobject] @{ ExitCode = $process.ExitCode; Output = $stdout.Result + $stderr.Result; TimedOut = $false }
    }

    if ((-not $AllowFailure) -and ($result.TimedOut -or ($result.ExitCode -ne 0))) {
        $what = if ($result.TimedOut) { "timed out after $TimeoutSec seconds" } else { "failed with exit code $($result.ExitCode)" }
        throw "$(Split-Path $FilePath -Leaf) $($psi.Arguments) $what`n$($result.Output.Trim())"
    }
    $result
}

function Invoke-VmRun {
    param (
        [Parameter(Mandatory)] [string[]] $Arguments,
        [switch] $Guest,
        [int] $TimeoutSec = 0,
        [switch] $AllowFailure
    )

    $vmrunArgs = @('-T', 'ws')
    if ($Guest) {
        $vmrunArgs += @('-gu', $LabDefaults.AdminUser, '-gp', $LabDefaults.AdminPassword)
    }
    Invoke-NativeCommand -FilePath (Get-VmwareTool 'vmrun.exe') -Arguments ($vmrunArgs + $Arguments) `
                         -TimeoutSec $TimeoutSec -AllowFailure:$AllowFailure
}

function Test-LabVmRunning {
    param ([Parameter(Mandatory)] [string] $Vmx)

    $list = Invoke-VmRun -Arguments @('list')
    $full = [System.IO.Path]::GetFullPath($Vmx)
    [bool] ($list.Output -split "`r?`n" | Where-Object { $_.Trim() -ieq $full })
}

function Test-GuestFile {
    param (
        [Parameter(Mandatory)] [string] $Vmx,
        [Parameter(Mandatory)] [string] $Path
    )

    $result = Invoke-VmRun -Guest -Arguments @('fileExistsInGuest', $Vmx, $Path) -TimeoutSec 60 -AllowFailure
    ($result.ExitCode -eq 0) -and ($result.Output -match 'exists')
}

function Wait-LabGuest {
    param (
        [Parameter(Mandatory)] [string] $Vmx,
        [string] $MarkerFile = "$GuestLabDir\ready.txt",
        [int] $TimeoutSec = 900
    )

    #
    # Wait until guest operations work with the lab credentials, which needs
    # VMware Tools to run in the guest, and the given marker file exists. Note
    # that "vmrun checkToolsState" is no use for this: recent versions report
    # "installed" even while Tools runs.
    #
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (Test-GuestFile -Vmx $Vmx -Path $MarkerFile) {
            return
        }
        Start-Sleep -Seconds 15
    }
    throw "The guest did not become ready within $TimeoutSec seconds (waiting for $MarkerFile)."
}

function Test-HostNestedVirtualization {
    #
    # VMware Workstation can only expose VT-x to a guest ("Virtualize Intel
    # VT-x/EPT") when it owns VT-x itself. If Hyper-V runs on the host, which
    # includes VBS features such as Memory Integrity and Credential Guard,
    # VMware runs on top of the Windows Hypervisor Platform instead, and cannot
    # nest. SimpleVisor then has no VT-x to load on in the guest.
    #
    $cs = Get-CimInstance -ClassName Win32_ComputerSystem
    if (-not $cs.HypervisorPresent) {
        return $true
    }

    Write-Warning @'
A hypervisor (Hyper-V or VBS) is running on this host. VMware Workstation then
cannot virtualize VT-x for its guests, so SimpleVisor will not load in the lab
VM. To fix this, on the host:
  - turn off Core isolation > Memory integrity, and Credential Guard
  - disable the Hyper-V, Virtual Machine Platform and Windows Hypervisor
    Platform features
  - run "bcdedit /set hypervisorlaunchtype off" (elevated) and reboot
Managed devices may enforce VBS through policy, in which case use a machine
without it.
'@
    $false
}

function Get-WimImage {
    param ([Parameter(Mandatory)] [string] $Path)

    #
    # Read the edition list straight from the XML metadata of install.wim or
    # install.esd, which, unlike Get-WindowsImage, needs no elevation. Its
    # resource header is at offset 48h of the WIM header: a 7-byte size with
    # a flags byte, then the offset; the XML itself is uncompressed UTF-16.
    #
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $reader = New-Object System.IO.BinaryReader($stream)
        $stream.Position = 0x48
        $size = $reader.ReadUInt64() -band 0x00FFFFFFFFFFFFFF
        $stream.Position = $reader.ReadInt64()
        $bytes = $reader.ReadBytes([int] $size)
    }
    finally {
        $stream.Dispose()
    }
    $xml = [xml] [System.Text.Encoding]::Unicode.GetString($bytes).TrimStart([char] 0xFEFF)
    $xml.WIM.IMAGE | ForEach-Object {
        [pscustomobject] @{ ImageIndex = [int] $_.INDEX; ImageName = $_.NAME }
    }
}

function Get-IsoImage {
    param ([Parameter(Mandatory)] [string] $Path)

    $mounted = Mount-DiskImage -ImagePath $Path -PassThru
    try {
        $drive = ($mounted | Get-Volume).DriveLetter + ':\'
        $wim = Get-ChildItem -Path (Join-Path $drive 'sources\install.*') |
               Where-Object { $_.Extension -in '.wim', '.esd' } |
               Select-Object -First 1
        if (-not $wim) {
            throw "$Path has no sources\install.wim or install.esd."
        }
        Get-WimImage -Path $wim.FullName
    }
    finally {
        Dismount-DiskImage -ImagePath $Path | Out-Null
    }
}

function Resolve-LabImage {
    param (
        [Parameter(Mandatory)] [string] $IsoPath,
        [int] $ImageIndex = 1,
        [string] $ImageName
    )

    #
    # Pick the edition to install, by name if one is given, and by index
    # otherwise
    #
    $images = @(Get-IsoImage -Path $IsoPath)
    if ($ImageName) {
        $image = $images | Where-Object { $_.ImageName -eq $ImageName }
        if (-not $image) {
            throw "$IsoPath has no edition named '$ImageName'. Use -ListImages to see them."
        }
    }
    else {
        $image = $images | Where-Object { $_.ImageIndex -eq $ImageIndex }
        if (-not $image) {
            throw "$IsoPath has no edition with index $ImageIndex. Use -ListImages to see them."
        }
    }
    $image
}

function Find-Oscdimg {
    param ([string] $OscdimgPath)

    if ($OscdimgPath) {
        return $OscdimgPath
    }
    $candidates = @(
        "${env:ProgramFiles(x86)}\Windows Kits\10\Assessment and Deployment Kit\Deployment Tools\amd64\Oscdimg\oscdimg.exe",
        (Join-Path $LabDefaults.WdkRoot 'Program Files\Windows Kits\10\Assessment and Deployment Kit\Deployment Tools\amd64\Oscdimg\oscdimg.exe')
    )
    $found = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $found) {
        throw 'oscdimg.exe was not found. Install the Windows ADK "Deployment Tools" feature, or pass -OscdimgPath.'
    }
    $found
}

function New-UnattendedIso {
    param (
        [Parameter(Mandatory)] [string] $SourceIso,
        [Parameter(Mandatory)] [string] $DestinationIso,
        [Parameter(Mandatory)] [int] $ImageIndex,
        [Parameter(Mandatory)] [string] $ComputerName,
        [string] $ProductKey,
        [string] $Locale = 'en-US',
        [hashtable[]] $ExtraFiles = @(),
        [string] $OscdimgPath
    )

    #
    # Windows install media boots under UEFI with a "Press any key to boot from
    # CD or DVD" prompt, which would stall an unattended install. Rebuild the
    # ISO with the no-prompt UEFI boot image, and with the answer file and the
    # first-logon script added to it, in a vmlab directory. ExtraFiles adds more
    # to that directory, as @{ Source = <file or directory>; Destination =
    # <path relative to vmlab> } entries.
    #
    $oscdimg = Find-Oscdimg -OscdimgPath $OscdimgPath
    $work = Join-Path ([System.IO.Path]::GetTempPath()) "shvlab-iso-$PID"
    $image = Mount-DiskImage -ImagePath $SourceIso -PassThru
    try {
        $source = (($image | Get-Volume).DriveLetter + ':\')
        Write-Host "Copying the install media from $source..."
        $copy = Invoke-NativeCommand -FilePath 'robocopy.exe' -Arguments @($source, $work, '/E', '/NFL', '/NDL', '/NJH', '/NJS', '/R:1', '/W:1') -AllowFailure
        if ($copy.ExitCode -ge 8) {
            throw "Copying the install media failed:`n$($copy.Output)"
        }
    }
    finally {
        Dismount-DiskImage -ImagePath $SourceIso | Out-Null
    }

    try {
        Get-ChildItem -Path $work -Recurse -File | ForEach-Object { $_.IsReadOnly = $false }

        $productKeyXml = ''
        if ($ProductKey) {
            $productKeyXml = "`n        <ProductKey>`n          <Key>$ProductKey</Key>`n          <WillShowUI>OnError</WillShowUI>`n        </ProductKey>"
        }
        $unattend = Get-Content -Raw -Path (Join-Path $PSScriptRoot 'unattend\autounattend.xml')
        $unattend = $unattend.Replace('{{LOCALE}}', $Locale).
                              Replace('{{IMAGE_INDEX}}', [string] $ImageIndex).
                              Replace('{{PRODUCT_KEY}}', $productKeyXml).
                              Replace('{{COMPUTER_NAME}}', $ComputerName.Substring(0, [Math]::Min(15, $ComputerName.Length))).
                              Replace('{{ADMIN_PASSWORD}}', [System.Security.SecurityElement]::Escape($LabDefaults.AdminPassword))
        [System.IO.File]::WriteAllText((Join-Path $work 'autounattend.xml'), $unattend)

        $labMedia = Join-Path $work 'vmlab'
        New-Item -ItemType Directory -Path $labMedia -Force | Out-Null
        Copy-Item -Path (Join-Path $PSScriptRoot 'guest\FirstLogon.ps1') -Destination $labMedia
        foreach ($extra in $ExtraFiles) {
            Copy-Item -Path $extra.Source -Destination (Join-Path $labMedia $extra.Destination) -Recurse
        }

        $etfsboot = Join-Path $work 'boot\etfsboot.com'
        $efisys = Join-Path $work 'efi\microsoft\boot\efisys_noprompt.bin'
        if (-not (Test-Path $efisys)) {
            $efisys = Join-Path (Split-Path $oscdimg -Parent) 'efisys_noprompt.bin'
        }

        Write-Host "Building the unattended ISO $DestinationIso..."
        Invoke-NativeCommand -FilePath $oscdimg -Arguments @(
            '-m', '-o', '-u2', '-udfver102', '-lSHVLAB',
            "-bootdata:2#p0,e,b$etfsboot#pEF,e,b$efisys",
            $work, $DestinationIso) | Out-Null
    }
    finally {
        Remove-Item -Path $work -Recurse -Force -ErrorAction SilentlyContinue
    }
}

function Read-LabResult {
    param ([Parameter(Mandatory)] [string] $Path)

    #
    # Parse the "phase.key=value" lines that guest\Test-ShvGuest.ps1 writes
    #
    $result = @{}
    foreach ($line in Get-Content -Path $Path) {
        $key, $value = $line -split '=', 2
        $result[$key] = $value
    }
    $result
}

function Show-LabResult {
    param (
        [Parameter(Mandatory)] [hashtable] $Result,
        [Parameter(Mandatory)] [string] $ResultFile,
        $KdCheck,
        [string] $NoVmxHint
    )

    #
    # Check the results of a guest test run, print them, and return the number
    # of failed checks
    #
    $get = {
        param ([string] $Key)
        if ($Result.ContainsKey($Key)) { $Result[$Key] } else { '<missing>' }
    }
    $checks = @(
        @{ Name = 'Guest test ran to completion';                       Pass = (& $get 'done') -eq '1' }
        @{ Name = 'Kernel debugging enabled on COM1 at 115200 baud';    Pass = ((& $get 'kd.debug') -eq 'Yes') -and ((& $get 'kd.debugtype') -eq 'Serial') -and
                                                                               ((& $get 'kd.debugport') -eq '1') -and ((& $get 'kd.baudrate') -eq '115200') }
        @{ Name = 'Guest has VMX or SVM';                               Pass = ((& $get 'before.vmx') -eq '1') -or ((& $get 'before.svm') -eq '1') }
        @{ Name = 'SimpleVisor not detected before loading';            Pass = (& $get 'before.simplevisor') -eq '0' }
        @{ Name = 'sc start simplevisor succeeded';                     Pass = (& $get 'load.exit') -eq '0' }
        @{ Name = 'SimpleVisor detected while loaded';                  Pass = (& $get 'loaded.simplevisor') -eq '1' }
        @{ Name = 'Hypervisor Present-bit set while loaded';            Pass = (& $get 'loaded.hypervisor_present') -eq '1' }
        @{ Name = 'Hypervisor vendor leaf unchanged while loaded';      Pass = (& $get 'loaded.hv_vendor') -eq (& $get 'before.hv_vendor') }
        @{ Name = 'Hypervisor interface leaf unchanged while loaded';   Pass = (& $get 'loaded.hv_interface') -eq (& $get 'before.hv_interface') }
        @{ Name = 'WAITPKG hidden, or usable, while loaded';            Pass = (& $get 'loaded.waitpkg_exec') -in @('ok', 'skipped') }
        @{ Name = 'SVM bit unchanged while loaded';                     Pass = (& $get 'loaded.svm') -eq (& $get 'before.svm') }
        @{ Name = 'VM-Exit counters readable while loaded';             Pass = ((& $get 'loaded.exits.processors') -match '^[1-9]') -and
                                                                               ((& $get 'loaded.exits.total') -match '^[1-9]') }
        @{ Name = 'Web browser started while loaded';                   Pass = (& $get 'browser.loaded') -match '^[1-9]' }
        @{ Name = 'sc stop simplevisor succeeded';                      Pass = (& $get 'unload.exit') -eq '0' }
        @{ Name = 'SimpleVisor not detected after unloading';           Pass = (& $get 'after.simplevisor') -eq '0' }
        @{ Name = 'Web browser kept running after unloading';           Pass = (& $get 'browser.after') -match '^[1-9]' }
        @{ Name = 'Web browser stopped';                                Pass = (& $get 'browser.stopped') -eq '0' }
    )
    if ($KdCheck) {
        $checks = @(@{ Name = 'kd broke in over the serial pipe, and resumed'; Pass = $KdCheck.Connected -and $KdCheck.Resumed }) + $checks
    }

    Write-Host ''
    Write-Host ("Underlying hypervisor: {0} ({1}); WAITPKG before/loaded: {2}/{3} ({4})" -f
        (& $get 'before.hv_vendor'), (& $get 'before.hv_interface'),
        (& $get 'before.waitpkg'), (& $get 'loaded.waitpkg'), (& $get 'loaded.waitpkg_exec'))

    #
    # The VM-Exits SimpleVisor handled until shvcheck.exe ran while it was
    # loaded, per exit reason (EXIT_REASON_* in vmx.h, or ShvSvmExitIndex
    # for SVM)
    #
    if ($Result.ContainsKey('loaded.exits.total')) {
        $reasons = $Result.Keys | Where-Object { $_ -match '^loaded\.exits\.(\d+)$' } |
                   Sort-Object { [int] ($_ -split '\.')[2] } |
                   ForEach-Object { "$(($_ -split '\.')[2]): $($Result[$_])" }
        Write-Host ("VM-Exits while loaded: {0} on {1} processors ({2})" -f
            (& $get 'loaded.exits.total'), (& $get 'loaded.exits.processors'), ($reasons -join ', '))
    }

    #
    # The TLB flush hypercalls SimpleVisor forwarded to Hyper-V by then
    #
    $tlbCalls = $Result.Keys | Where-Object { $_ -match '^loaded\.tlbcalls\.' } | Sort-Object |
                ForEach-Object { "$(($_ -split '\.')[2]): $($Result[$_])" }
    if ($tlbCalls) {
        Write-Host ("TLB flush hypercalls while loaded: {0}" -f ($tlbCalls -join ', '))
    }
    if ((& $get 'load.exit') -ne '0') {
        Write-Host "sc start: $(& $get 'load.output')"
    }
    if ($Result.ContainsKey('browser.output')) {
        Write-Host "Web browser: $(& $get 'browser.output')"
    }
    if (((& $get 'before.vmx') -ne '1') -and ((& $get 'before.svm') -ne '1')) {
        Write-Warning "The guest has neither VMX nor SVM, so SimpleVisor cannot load in it. $NoVmxHint"
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
    Write-Host "Full results: $ResultFile"
    $failed
}

function Get-LabDebugger {
    param (
        [string] $WdkRoot = $LabDefaults.WdkRoot,
        [switch] $Console
    )

    $name = if ($Console) { 'kd.exe' } else { 'windbg.exe' }
    $path = Join-Path $WdkRoot "Program Files\Windows Kits\10\Debuggers\x64\$name"
    if (-not (Test-Path $path)) {
        throw "The debugger was not found at $path. Pass -WdkRoot, or set SHV_WDK_ROOT."
    }
    $path
}

function Get-LabDebuggerArguments {
    param ([string] $PipeName = $LabDefaults.PipeName)

    #
    # Note that kd must be allowed to send its reset packets on connecting
    # (i.e. no "resets=0"): without them, a running target stays silent, and kd
    # never gets to break in.
    #
    $symbolCache = Join-Path ([System.IO.Path]::GetTempPath()) 'symbols'
    @('-k', "com:pipe,port=\\.\pipe\$PipeName,reconnect",
      '-y', "$($LabDefaults.OutDir);srv*$symbolCache*https://msdl.microsoft.com/download/symbols")
}

function Send-LabKdBreakIn {
    param (
        [string] $PipeName = $LabDefaults.PipeName,
        [int] $TimeoutSec = 30
    )

    #
    # Break into the guest's kernel debugger by writing the KD break-in byte
    # ('b') to the VM's serial pipe directly, until the target reports that it
    # stopped, with a state change packet: leader "0000", then packet type 7
    # (PACKET_TYPE_KD_STATE_CHANGE64). kd's own -b is unreliable here: it sends
    # the break-in once, early, and a running target that misses it stays
    # silent. The target then waits, broken in, for kd to attach.
    #
    # Send as few break-in bytes as possible. A target that is broken in still
    # takes a break-in byte as a request to break in again, right after kd
    # resumes it. So first listen for a while, as a target that is already
    # broken in keeps resending its state change, and only then break in, once.
    #
    $pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', $PipeName,
                [System.IO.Pipes.PipeDirection]::InOut, [System.IO.Pipes.PipeOptions]::Asynchronous)
    try {
        $pipe.Connect($TimeoutSec * 1000)
        $buffer = New-Object byte[] 4096
        $received = New-Object System.Text.StringBuilder
        $read = $pipe.ReadAsync($buffer, 0, $buffer.Length)
        $deadline = (Get-Date).AddSeconds($TimeoutSec)
        $nextBreakIn = (Get-Date).AddSeconds(5)
        while ((Get-Date) -lt $deadline) {
            if ((Get-Date) -ge $nextBreakIn) {
                $pipe.Write([byte[]] @(0x62), 0, 1)
                $pipe.Flush()
                $nextBreakIn = (Get-Date).AddSeconds(10)
            }
            if ($read.Wait(500)) {
                if ($read.Result -eq 0) {
                    break
                }
                [void] $received.Append([System.Text.Encoding]::GetEncoding(28591).GetString($buffer, 0, $read.Result))
                if ($received.ToString().Contains("0000$([char] 7)$([char] 0)")) {
                    return $true
                }
                $read = $pipe.ReadAsync($buffer, 0, $buffer.Length)
            }
        }
        $false
    }
    catch {
        $false
    }
    finally {
        $pipe.Dispose()
    }
}

function Test-LabKernelDebugger {
    param (
        [string] $Vmx = (Get-LabVmx),
        [string] $PipeName = $LabDefaults.PipeName,
        [scriptblock] $TestGuest,
        [string] $WdkRoot = $LabDefaults.WdkRoot,
        [int] $TimeoutSec = 120,
        [switch] $KeepAttached,
        [string] $LogPath
    )

    #
    # Check, end to end, that the kernel debugger works over the VM's serial
    # pipe: break in, attach kd, identify the target, and resume it, and then
    # confirm that the guest runs again before closing kd. Closing kd leaves the
    # target running, unlike "q", which would leave it broken in, but only once
    # kd got to send "g", which can take a while. KD tolerates the debugger
    # coming and going, so the pipe stays usable for Connect-LabDebugger.ps1.
    #
    # TestGuest tells whether the guest responds, and defaults to a VMware
    # guest operation on the given VM.
    #
    # With KeepAttached, kd stays attached in its own window and logs to
    # LogPath. Stop it with Stop-LabKernelDebugger.
    #
    if (-not $TestGuest) {
        $TestGuest = { Test-GuestFile -Vmx $Vmx -Path "$GuestLabDir\ready.txt" }.GetNewClosure()
    }
    $notResumed = "`nThe guest may be left broken into the debugger; attach with Connect-LabDebugger.ps1 to resume it."
    if (-not (Send-LabKdBreakIn -PipeName $PipeName)) {
        return [pscustomobject] @{
            Connected = $false
            Resumed   = $false
            Target    = ''
            Log       = 'The guest kernel did not answer a KD break-in on its serial pipe.'
        }
    }

    $marker = 'SHVLAB-KD-CONNECTED'
    $log = if ($LogPath) { $LogPath } else { Join-Path ([System.IO.Path]::GetTempPath()) "shvlab-kd-$PID.log" }
    Remove-Item -Path $log -ErrorAction SilentlyContinue
    $arguments = (Get-LabDebuggerArguments -PipeName $PipeName) + @('-logo', $log, '-c', "vertarget; .echo $marker; g")

    #
    # Symbols are not needed for this, so keep kd from going through whatever
    # symbol path the environment sets, which may include slow network shares
    #
    $symbolPath = $env:_NT_SYMBOL_PATH
    $env:_NT_SYMBOL_PATH = $null
    try {
        $kd = Start-Process -FilePath (Get-LabDebugger -WdkRoot $WdkRoot -Console) -PassThru `
                            -WindowStyle $(if ($KeepAttached) { 'Normal' } else { 'Hidden' }) `
                            -ArgumentList (($arguments | ForEach-Object { ConvertTo-CommandLineArgument $_ }) -join ' ')
    }
    finally {
        $env:_NT_SYMBOL_PATH = $symbolPath
    }

    $connected = $false
    $resumed = $false
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    try {
        while ((Get-Date) -lt $deadline -and -not $kd.HasExited) {
            if ((-not $connected) -and (Test-Path $log) -and ((Get-Content -Raw -Path $log) -match $marker)) {
                $connected = $true
            }

            #
            # Once kd ran the commands, wait for the guest to run again, as
            # shown by a guest operation succeeding
            #
            if ($connected -and (& $TestGuest)) {
                $resumed = $true
                break
            }
            Start-Sleep -Seconds 2
        }
    }
    finally {
        if ((-not $kd.HasExited) -and -not ($KeepAttached -and $resumed)) {
            Stop-Process -Id $kd.Id -Force
        }
    }

    $text = if (Test-Path $log) { Get-Content -Raw -Path $log } else { '' }
    if (-not $KeepAttached) {
        Remove-Item -Path $log -ErrorAction SilentlyContinue
    }
    if (-not $resumed) {
        $text += $notResumed
    }
    [pscustomobject] @{
        Connected = $connected
        Resumed   = $resumed
        Target    = ([regex]::Match($text, '(?m)^Windows .*Kernel Version.*$')).Value.Trim()
        Log       = $text
        LogPath   = $log
        Process   = if ($KeepAttached -and $resumed) { $kd } else { $null }
    }
}

function Show-LabDebuggerOutput {
    param ([Parameter(Mandatory)] $KdCheck)

    #
    # Show what the guest printed to the attached kd since it connected
    #
    $text = if (Test-Path $KdCheck.LogPath) { Get-Content -Raw -Path $KdCheck.LogPath } else { '' }
    $index = $text.LastIndexOf('SHVLAB-KD-CONNECTED')
    if ($index -lt 0) {
        return
    }
    Write-Host ''
    Write-Host "Kernel debugger output (full log: $($KdCheck.LogPath)):"
    foreach ($line in ($text.Substring($index) -split '\r?\n' | Select-Object -Skip 1)) {
        if ($line.Trim()) {
            Write-Host "  $line"
        }
    }
}

function Stop-LabKernelDebugger {
    param ($KdCheck)

    if ($KdCheck -and $KdCheck.Process -and -not $KdCheck.Process.HasExited) {
        Stop-Process -Id $KdCheck.Process.Id -Force
    }
}

function Save-LabScreenshot {
    param (
        [Parameter(Mandatory)] [string] $Path,
        [int] $Port = $LabDefaults.VncPort,
        [int] $TimeoutSec = 30
    )

    #
    # Save the VM console as a PNG, through the VNC server that VMware runs for
    # the VM on the loopback interface. Unlike "vmrun captureScreen", this does
    # not need VMware Tools, so it also works while the guest is still being
    # installed, or after it hung or bugchecked.
    #
    # This is a minimal RFB 3.8 client: no authentication, 32bpp true color in
    # BGRX order, and raw encoding, requesting one full framebuffer update.
    #
    Add-Type -AssemblyName System.Drawing
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $client.ReceiveTimeout = $TimeoutSec * 1000
        $client.Connect('127.0.0.1', $Port)
        $stream = $client.GetStream()

        $readExact = {
            param ([int] $Count)
            $buffer = New-Object byte[] $Count
            $offset = 0
            while ($offset -lt $Count) {
                $read = $stream.Read($buffer, $offset, $Count - $offset)
                if ($read -le 0) { throw 'The VNC server closed the connection.' }
                $offset += $read
            }
            , $buffer
        }
        $u16 = { param ($b, $i) ([int] $b[$i] -shl 8) -bor $b[$i + 1] }
        $u32 = { param ($b, $i) ([uint32] $b[$i] -shl 24) -bor ([uint32] $b[$i + 1] -shl 16) -bor ([uint32] $b[$i + 2] -shl 8) -bor $b[$i + 3] }
        $send = { param ([byte[]] $Bytes) $stream.Write($Bytes, 0, $Bytes.Length) }

        [void] (& $readExact 12)
        & $send ([System.Text.Encoding]::ASCII.GetBytes("RFB 003.008`n"))
        $types = & $readExact ((& $readExact 1)[0])
        if ($types -notcontains 1) {
            throw 'The VNC server requires authentication.'
        }
        & $send ([byte[]] @(1))
        if ((& $u32 (& $readExact 4) 0) -ne 0) {
            throw 'The VNC server rejected the connection.'
        }
        & $send ([byte[]] @(1))

        $init = & $readExact 24
        $width = & $u16 $init 0
        $height = & $u16 $init 2
        [void] (& $readExact (& $u32 $init 20))

        & $send ([byte[]] @(0, 0, 0, 0, 32, 24, 0, 1, 0, 255, 0, 255, 0, 255, 16, 8, 0, 0, 0, 0))
        & $send ([byte[]] @(2, 0, 0, 1, 0, 0, 0, 0))
        & $send ([byte[]] @(3, 0, 0, 0, 0, 0, ($width -shr 8), ($width -band 0xFF), ($height -shr 8), ($height -band 0xFF)))

        $bitmap = New-Object System.Drawing.Bitmap $width, $height, ([System.Drawing.Imaging.PixelFormat]::Format32bppRgb)
        try {
            while ($true) {
                $type = (& $readExact 1)[0]
                if ($type -eq 0) {
                    break
                }
                switch ($type) {
                    2 { }
                    3 { $header = & $readExact 7; [void] (& $readExact (& $u32 $header 3)) }
                    default { throw "Unexpected VNC message type $type." }
                }
            }

            $rectangles = & $u16 (& $readExact 3) 1
            for ($i = 0; $i -lt $rectangles; $i++) {
                $rect = & $readExact 12
                $x = & $u16 $rect 0
                $y = & $u16 $rect 2
                $w = & $u16 $rect 4
                $h = & $u16 $rect 6
                if ((& $u32 $rect 8) -ne 0) {
                    throw 'The VNC server sent a non-raw rectangle.'
                }
                if (($w -eq 0) -or ($h -eq 0)) {
                    continue
                }
                $pixels = & $readExact ($w * $h * 4)
                $area = New-Object System.Drawing.Rectangle $x, $y, $w, $h
                $data = $bitmap.LockBits($area, [System.Drawing.Imaging.ImageLockMode]::WriteOnly, $bitmap.PixelFormat)
                try {
                    for ($row = 0; $row -lt $h; $row++) {
                        [System.Runtime.InteropServices.Marshal]::Copy($pixels, $row * $w * 4,
                            [IntPtr] ($data.Scan0.ToInt64() + $row * $data.Stride), $w * 4)
                    }
                }
                finally {
                    $bitmap.UnlockBits($data)
                }
            }
            $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
        }
        finally {
            $bitmap.Dispose()
        }
    }
    finally {
        $client.Dispose()
    }
}

function Send-LabKeys {
    param (
        [string] $Text = '',
        [switch] $RunDialog,
        [switch] $Enter,
        [int] $Port = $LabDefaults.VncPort,
        [switch] $ScancodeWinKey,
        [int] $DelayMs = 15
    )

    #
    # Type into the VM console over VNC (RFB key events), to reach the guest
    # when neither VMware Tools nor the network work, e.g. to open a console:
    #
    #   Send-LabKeys -RunDialog -Text 'powershell' -Enter
    #
    # RunDialog presses Win+R first. VMware maps the Super_L keysym to the
    # Windows key, while OpenVMM has no keysym for it, and needs the scancode
    # (E0 5B) as a QEMU extended key event instead: pass -ScancodeWinKey. Shift
    # is pressed explicitly for shifted characters, which VMware requires.
    #
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $client.ReceiveTimeout = 10000
        $client.Connect('127.0.0.1', $Port)
        $stream = $client.GetStream()

        $readExact = {
            param ([int] $Count)
            $buffer = New-Object byte[] $Count
            $offset = 0
            while ($offset -lt $Count) {
                $read = $stream.Read($buffer, $offset, $Count - $offset)
                if ($read -le 0) { throw 'The VNC server closed the connection.' }
                $offset += $read
            }
            , $buffer
        }
        $u32 = { param ($b, $i) ([uint32] $b[$i] -shl 24) -bor ([uint32] $b[$i + 1] -shl 16) -bor ([uint32] $b[$i + 2] -shl 8) -bor $b[$i + 3] }
        $send = { param ([byte[]] $Bytes) $stream.Write($Bytes, 0, $Bytes.Length) }

        [void] (& $readExact 12)
        & $send ([System.Text.Encoding]::ASCII.GetBytes("RFB 003.008`n"))
        [void] (& $readExact ((& $readExact 1)[0]))
        & $send ([byte[]] @(1))
        [void] (& $readExact 4)
        & $send ([byte[]] @(1))
        $init = & $readExact 24
        [void] (& $readExact (& $u32 $init 20))

        $key = {
            param ([uint32] $Sym, [int] $Down)
            & $send ([byte[]] @(4, $Down, 0, 0, (($Sym -shr 24) -band 0xFF), (($Sym -shr 16) -band 0xFF), (($Sym -shr 8) -band 0xFF), ($Sym -band 0xFF)))
            Start-Sleep -Milliseconds $DelayMs
        }
        $scancode = {
            param ([uint32] $Code, [int] $Down)
            & $send ([byte[]] @(255, 0, 0, $Down, 0, 0, 0, 0, 0, 0, 0, $Code))
            Start-Sleep -Milliseconds $DelayMs
        }

        if ($RunDialog) {
            if ($ScancodeWinKey) {
                #
                # Advertise the QEMU extended key event pseudo-encoding (-258),
                # then press E0 5B (0xDB in QEMU's numbering) with R (0x13)
                #
                & $send ([byte[]] @(2, 0, 0, 2, 0, 0, 0, 0, 0xFF, 0xFF, 0xFE, 0xFE))
                & $scancode 0xDB 1; & $scancode 0x13 1; & $scancode 0x13 0; & $scancode 0xDB 0
            }
            else {
                & $key 0xFFEB 1; & $key 0x72 1; & $key 0x72 0; & $key 0xFFEB 0
            }
            Start-Sleep -Milliseconds 800
        }

        $shifted = '~!@#$%^&*()_+{}|:"<>?ABCDEFGHIJKLMNOPQRSTUVWXYZ'
        foreach ($char in $Text.ToCharArray()) {
            $shift = $shifted.Contains($char)
            if ($shift) { & $key 0xFFE1 1 }
            & $key ([uint32] $char) 1
            & $key ([uint32] $char) 0
            if ($shift) { & $key 0xFFE1 0 }
        }
        if ($Enter) {
            & $key 0xFF0D 1
            & $key 0xFF0D 0
        }
        Start-Sleep -Milliseconds 300
    }
    finally {
        $client.Dispose()
    }
}
