#
# Settings and helpers for the OpenVMM flavor of the SimpleVisor lab, on top of
# ..\LabCommon.ps1. This file is dot-sourced by the scripts next to it, and is
# not meant to be run on its own.
#
# OpenVMM runs on the Windows Hypervisor Platform (WHP), and, unlike VMware
# Workstation, can expose VT-x to its guest even when the host runs Hyper-V or
# VBS (--nested-virt). WHP does not allow that together with VMBus, however.
# So the VM is installed and maintained with VMBus (for the install DVD and
# synthetic video), and tested without it, with VT-x exposed. Both
# configurations share the same PCIe NVMe disk and virtio NIC, so that Windows
# sees the same devices in both.
#

. (Join-Path (Split-Path $PSScriptRoot -Parent) 'LabCommon.ps1')

$script:OvmmRoot = if ($env:SHV_OPENVMM_ROOT) { $env:SHV_OPENVMM_ROOT } else { Join-Path (Split-Path $RepoRoot -Parent) 'openvmm' }
$script:OvmmDefaults = @{
    LabName  = 'SimpleVisorLabOvmm'
    LabDir   = Join-Path $LabDefaults.VmRoot 'SimpleVisorLabOvmm'
    Exe      = if ($env:SHV_OPENVMM_EXE) { $env:SHV_OPENVMM_EXE } else { Join-Path $OvmmRoot 'target\release\openvmm.exe' }
    Firmware = Join-Path $OvmmRoot '.packages\hyperv.uefi.mscoreuefi.x64.RELEASE\MsvmX64\RELEASE_VS2022\FV\MSVM.fd'
    NetKvm   = Join-Path $OvmmRoot '.packages\virtio-win\NetKVM\2k25\amd64'
    PipeName = 'SimpleVisorLabOvmm'
    SerialDebuggerMode = $false
    VncPort  = 5931
    SshPort  = 22222
    CpuCount = 4
    MemoryMB = 4096
}

function Get-OvmmLabFile {
    param ([Parameter(Mandatory)] [string] $Name)

    Join-Path $OvmmDefaults.LabDir $Name
}

function Assert-OvmmPrerequisite {
    #
    # openvmm.exe is built from the OpenVMM repo (cargo build --release), and
    # its UEFI firmware and the virtio-win drivers are packages that OpenVMM's
    # build tooling restores into the repo's .packages directory.
    #
    foreach ($item in @(
            @{ Path = $OvmmDefaults.Exe;      What = 'openvmm.exe' },
            @{ Path = $OvmmDefaults.Firmware; What = 'the OpenVMM UEFI firmware (MSVM.fd)' },
            @{ Path = (Join-Path $OvmmDefaults.NetKvm 'netkvm.inf'); What = 'the virtio-win NetKVM driver' })) {
        if (-not (Test-Path $item.Path)) {
            throw "$($item.What) was not found at $($item.Path). Set SHV_OPENVMM_ROOT (and SHV_OPENVMM_EXE) to your OpenVMM build."
        }
    }
}

function Get-OvmmArguments {
    param (
        [Parameter(Mandatory)] [ValidateSet('install', 'maintain', 'test')] [string] $Mode
    )

    #
    # The modes:
    #   install   VMBus, with the install DVD on VMBus SCSI (OpenVMM's IDE
    #             needs the PC/AT BIOS) and synthetic video
    #   maintain  the same without the DVD, which would rerun Setup, to boot
    #             and update the clean disk
    #   test      VT-x exposed and no VMBus, booting a throwaway in-memory diff
    #             of the clean disk, which is like reverting to a snapshot
    #
    # Common to all: UEFI without Secure Boot (so test signing works), the NVMe
    # disk and the virtio NIC on PCIe root ports, NAT with the guest's SSH port
    # forwarded to the loopback interface, COM1 on a named pipe for the kernel
    # debugger, and the console over VNC on the loopback interface.
    #
    # COM1 is a plain pipe by default, not in OpenVMM's "debugger-mode". That
    # mode throttles a guest that polls an empty receive FIFO, delaying each
    # status register read by 4 ms, but Windows KD waits for data in exactly
    # such loops. kd then attaches, but makes no progress for minutes, and boot
    # is three times slower (see serial_16550's DebuggerPollThrottle).
    #
    $disk = Get-OvmmLabFile 'disk.img'
    $diskSpec = if ($Mode -eq 'test') { "memdiff:file:$disk" } else { "file:$disk" }
    $com1 = "listen=//./pipe/$($OvmmDefaults.PipeName)"
    if ($OvmmDefaults.SerialDebuggerMode) {
        $com1 = "debugger-mode:$com1"
    }
    $arguments = @(
        '--uefi', "firmware=$($OvmmDefaults.Firmware),default_boot_always_attempt",
        '-p', [string] $OvmmDefaults.CpuCount, '-m', "$($OvmmDefaults.MemoryMB)M",
        '--pcie-root-complex', 'rc0',
        '--pcie-root-port', 'rc0:rp0', '--pcie-root-port', 'rc0:rp1',
        '--nvme', "$diskSpec,pcie_port=rp0",
        '--virtio-net', "pcie_port=rp1:consomme:hostfwd=tcp:127.0.0.1:$($OvmmDefaults.SshPort)-:22",
        '--com1', $com1,
        '--vnc', '--vnc-port', [string] $OvmmDefaults.VncPort,
        '--guest-shutdown-action', 'exit',
        '--log-file', (Get-OvmmLabFile "openvmm-$Mode.log"))
    switch ($Mode) {
        'install' {
            $arguments += @('--gfx', '--vmbus-scsi', 'id=scsi0',
                            '--disk', "file:$(Get-OvmmLabFile 'install.iso'),dvd,on=scsi0")
        }
        'maintain' {
            $arguments += @('--gfx')
        }
        'test' {
            $arguments += @('--nested-virt', '--no-vmbus')
        }
    }
    $arguments
}

function Get-OvmmLabVm {
    #
    # The OpenVMM process of this lab, if it runs, as recorded when it was
    # started. Checking its start time too keeps a reused process ID from being
    # mistaken for it.
    #
    $pidFile = Get-OvmmLabFile 'openvmm.pid'
    if (-not (Test-Path $pidFile)) {
        return $null
    }
    $id, $started = (Get-Content -Path $pidFile -Raw).Trim() -split ' ', 2
    $process = Get-Process -Id ([int] $id) -ErrorAction SilentlyContinue
    if ($process -and ($process.StartTime.ToFileTimeUtc() -eq [int64] $started)) {
        return $process
    }
    $null
}

function Start-OvmmLabVm {
    param (
        [Parameter(Mandatory)] [ValidateSet('install', 'maintain', 'test')] [string] $Mode
    )

    Assert-OvmmPrerequisite
    $running = Get-OvmmLabVm
    if ($running) {
        throw "The lab VM already runs (OpenVMM PID $($running.Id)). Stop it first with Stop-OvmmLabVm."
    }

    #
    # OpenVMM's interactive console reads stdin. Give it an empty file rather
    # than the console of whoever started it.
    #
    $stdin = Get-OvmmLabFile 'stdin.txt'
    [System.IO.File]::WriteAllText($stdin, '')
    $arguments = (Get-OvmmArguments -Mode $Mode | ForEach-Object { ConvertTo-CommandLineArgument $_ }) -join ' '
    $process = Start-Process -FilePath $OvmmDefaults.Exe -ArgumentList $arguments -PassThru -WindowStyle Hidden `
                             -RedirectStandardInput $stdin `
                             -RedirectStandardOutput (Get-OvmmLabFile "openvmm-$Mode.out") `
                             -RedirectStandardError (Get-OvmmLabFile "openvmm-$Mode.err")
    Set-Content -Path (Get-OvmmLabFile 'openvmm.pid') -Value "$($process.Id) $($process.StartTime.ToFileTimeUtc())"
    $process
}

function Stop-OvmmLabVm {
    #
    # OpenVMM runs the VM and its VNC server in child processes ("openvmm.exe
    # vm" and "openvmm.exe vnc"), so stop those too, not just the process that
    # was started
    #
    $process = Get-OvmmLabVm
    if ($process) {
        $children = Get-CimInstance -ClassName Win32_Process -Filter "ParentProcessId = $($process.Id)" |
                    Where-Object { $_.Name -eq 'openvmm.exe' }
        Stop-Process -Id $process.Id -Force
        foreach ($child in $children) {
            Stop-Process -Id $child.ProcessId -Force -ErrorAction SilentlyContinue
        }
        $process.WaitForExit()
    }
    Remove-Item -Path (Get-OvmmLabFile 'openvmm.pid') -ErrorAction SilentlyContinue
}

function Get-OvmmSshArguments {
    param ([switch] $Scp)

    #
    # Only offer the lab key: with whatever else ssh-agent holds, sshd may give
    # up with "Too many authentication failures" first. The guest's host key
    # changes with every install, so don't keep it.
    #
    $port = if ($Scp) { '-P' } else { '-p' }
    @('-i', (Get-OvmmLabFile 'id_lab'), $port, [string] $OvmmDefaults.SshPort,
      '-o', 'IdentitiesOnly=yes', '-o', 'StrictHostKeyChecking=no', '-o', 'UserKnownHostsFile=NUL',
      '-o', 'LogLevel=ERROR', '-o', 'ConnectTimeout=10', '-o', 'BatchMode=yes')
}

function Invoke-OvmmGuest {
    param (
        [Parameter(Mandatory)] [string] $Command,
        [int] $TimeoutSec = 120,
        [switch] $AllowFailure
    )

    #
    # Run a command in the guest over SSH, elevated, as the guest's built-in
    # Administrator. The default shell of Windows' OpenSSH server is cmd.exe.
    #
    $arguments = (Get-OvmmSshArguments) + @("$($LabDefaults.AdminUser)@127.0.0.1", $Command)
    Invoke-NativeCommand -FilePath "$env:SystemRoot\System32\OpenSSH\ssh.exe" -Arguments $arguments `
                         -TimeoutSec $TimeoutSec -AllowFailure:$AllowFailure
}

function Copy-OvmmToGuest {
    param (
        [Parameter(Mandatory)] [string] $Path,
        [Parameter(Mandatory)] [string] $Destination
    )

    $arguments = @('-q') + (Get-OvmmSshArguments -Scp) +
                 @($Path, "$($LabDefaults.AdminUser)@127.0.0.1:$($Destination -replace '\\', '/')")
    Invoke-NativeCommand -FilePath "$env:SystemRoot\System32\OpenSSH\scp.exe" -Arguments $arguments -TimeoutSec 120 | Out-Null
}

function Copy-OvmmFromGuest {
    param (
        [Parameter(Mandatory)] [string] $Path,
        [Parameter(Mandatory)] [string] $Destination
    )

    $arguments = @('-q') + (Get-OvmmSshArguments -Scp) +
                 @("$($LabDefaults.AdminUser)@127.0.0.1:$($Path -replace '\\', '/')", $Destination)
    Invoke-NativeCommand -FilePath "$env:SystemRoot\System32\OpenSSH\scp.exe" -Arguments $arguments -TimeoutSec 120 | Out-Null
}

function Test-OvmmGuestFile {
    param ([Parameter(Mandatory)] [string] $Path)

    $result = Invoke-OvmmGuest -Command "if exist $Path echo EXISTS" -TimeoutSec 30 -AllowFailure
    (-not $result.TimedOut) -and ($result.Output -match 'EXISTS')
}

function Wait-OvmmGuest {
    param (
        [System.Diagnostics.Process] $Process,
        [string] $MarkerFile = "$GuestLabDir\ready.txt",
        [int] $TimeoutSec = 900
    )

    #
    # Wait until SSH into the guest works and the given marker file exists,
    # giving up early if OpenVMM exits
    #
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if ($Process -and $Process.HasExited) {
            throw "OpenVMM exited with code $($Process.ExitCode); see the openvmm-*.err and .log files in $($OvmmDefaults.LabDir)."
        }
        if (Test-OvmmGuestFile -Path $MarkerFile) {
            return
        }
        Start-Sleep -Seconds 15
    }
    throw "The guest did not become ready within $TimeoutSec seconds (waiting for $MarkerFile)."
}

function Test-OvmmKernelDebugger {
    param ([int] $TimeoutSec = 120)

    Test-LabKernelDebugger -PipeName $OvmmDefaults.PipeName -TimeoutSec $TimeoutSec `
                           -TestGuest { Test-OvmmGuestFile -Path "$GuestLabDir\ready.txt" }
}
