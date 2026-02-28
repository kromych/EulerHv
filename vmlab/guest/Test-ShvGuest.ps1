#
# Runs inside the lab VM, elevated, from Invoke-ShvTest.ps1. It loads and
# unloads SimpleVisor, running shvcheck.exe before, during and after, and writes
# "phase.key=value" lines to C:\vmlab\result.txt for the host to check.
#

$ErrorActionPreference = 'Continue'
$labDir = 'C:\vmlab'
$resultFile = Join-Path $labDir 'result.txt'
$service = 'simplevisor'

function Write-Result {
    param ([string] $Line)

    Add-Content -Path $resultFile -Value $Line -Encoding Ascii
}

function Invoke-Phase {
    param ([string] $Phase)

    & (Join-Path $labDir 'shvcheck.exe') | ForEach-Object { Write-Result "$Phase.$_" }
}

Remove-Item -Path $resultFile -ErrorAction SilentlyContinue

#
# Record how the kernel debugger is configured, so the host can check that it
# is enabled on COM1, which the VM connects to the debugger pipe
#
$debugOn = (bcdedit /enum '{current}' | Select-String -Pattern '^debug\s+(\S+)').Matches |
           ForEach-Object { $_.Groups[1].Value }
Write-Result "kd.debug=$debugOn"
foreach ($line in bcdedit /dbgsettings) {
    if ($line -match '^(debugtype|debugport|baudrate)\s+(\S+)') {
        Write-Result "kd.$($Matches[1])=$($Matches[2])"
    }
}

#
# Trust the lab test-signing certificate. Test signing mode alone accepts the
# signature, but trusting it also keeps Code Integrity from logging failures.
#
$cert = Join-Path $labDir 'SimpleVisorLab.cer'
certutil -f -addstore Root $cert | Out-Null
certutil -f -addstore TrustedPublisher $cert | Out-Null

#
# (Re)create the driver service
#
sc.exe stop $service 2>&1 | Out-Null
sc.exe delete $service 2>&1 | Out-Null
sc.exe create $service type= kernel binPath= (Join-Path $labDir 'shv.sys') | Out-Null

Invoke-Phase 'before'

#
# Loading virtualizes every processor. If SimpleVisor halts, this never returns,
# and the host notices through its timeout.
#
$output = sc.exe start $service 2>&1
Write-Result "load.exit=$LASTEXITCODE"
Write-Result "load.output=$(($output | Where-Object { $_ }) -join ' | ')"

Invoke-Phase 'loaded'

$output = sc.exe stop $service 2>&1
Write-Result "unload.exit=$LASTEXITCODE"
Write-Result "unload.output=$(($output | Where-Object { $_ }) -join ' | ')"

Invoke-Phase 'after'

sc.exe delete $service | Out-Null
Write-Result 'done=1'
