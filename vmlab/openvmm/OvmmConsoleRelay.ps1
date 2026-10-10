#
# Runs OpenVMM for the lab, started hidden by Start-OvmmLabVm, and keeps its
# interactive console reachable while the VM runs. OpenVMM reads console
# commands from stdin, so this holds its stdin, and forwards to it the lines
# written to a named pipe, which Invoke-OvmmConsole does. Its stdout, where the
# commands answer, and stderr go to files. The relay exits with OpenVMM.
#
# OpenVMM's command line comes from a file, so that its quoting survives
#
param (
    [Parameter(Mandatory)] [string] $Exe,
    [Parameter(Mandatory)] [string] $ArgumentsFile,
    [Parameter(Mandatory)] [string] $StdOut,
    [Parameter(Mandatory)] [string] $StdErr,
    [Parameter(Mandatory)] [string] $PidFile,
    [Parameter(Mandatory)] [string] $PipeName
)

$info = New-Object System.Diagnostics.ProcessStartInfo
$info.FileName = $Exe
$info.Arguments = [System.IO.File]::ReadAllText($ArgumentsFile)
$info.UseShellExecute = $false
$info.RedirectStandardInput = $true
$info.RedirectStandardOutput = $true
$info.RedirectStandardError = $true
$process = [System.Diagnostics.Process]::Start($info)

#
# Unbuffered, so that a command's answer is in the file as soon as OpenVMM
# writes it, and shared, so that it can be read meanwhile
#
$out = New-Object System.IO.FileStream($StdOut, 'Create', 'Write', 'ReadWrite', 1)
$err = New-Object System.IO.FileStream($StdErr, 'Create', 'Write', 'ReadWrite', 1)
$copies = @($process.StandardOutput.BaseStream.CopyToAsync($out),
            $process.StandardError.BaseStream.CopyToAsync($err))
Set-Content -Path $PidFile -Value "$($process.Id) $($process.StartTime.ToFileTimeUtc())"

$stdin = $process.StandardInput
$stdin.AutoFlush = $true
while (-not $process.HasExited) {
    #
    # One client at a time, each writing one or more command lines. The
    # pipe's default security lets only this user and administrators write
    #
    $pipe = New-Object System.IO.Pipes.NamedPipeServerStream($PipeName, 'In', 1, 'Byte', 'Asynchronous')
    try {
        $connect = $pipe.WaitForConnectionAsync()
        while (-not $connect.Wait(1000)) {
            if ($process.HasExited) {
                break
            }
        }
        if ($connect.IsCompleted -and -not $connect.IsFaulted) {
            $reader = New-Object System.IO.StreamReader($pipe)
            while ($null -ne ($line = $reader.ReadLine())) {
                $stdin.WriteLine($line)
            }
        }
    }
    catch {
        # The client went away mid-line; wait for the next one
    }
    finally {
        $pipe.Dispose()
    }
}
$process.WaitForExit()
[void] [System.Threading.Tasks.Task]::WaitAll([System.Threading.Tasks.Task[]] $copies, 10000)
$out.Dispose()
$err.Dispose()
