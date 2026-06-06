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

function Get-BrowserCount {
    @(Get-Process -Name 'msedge' -ErrorAction SilentlyContinue).Count
}

function Start-Browser {
    #
    # Start Edge on the console, in the session of the automatic logon, as
    # PowerShell Direct and SSH sessions have no desktop. It opens a local
    # page that keeps drawing and allocating, as the VM may have no network,
    # so that the browser maps and unmaps memory all the while
    #
    $page = Join-Path $labDir 'browser.html'
    Set-Content -Path $page -Encoding Ascii -Value @'
<html><body><canvas id="c" width="800" height="600"></canvas><script>
var c = document.getElementById("c").getContext("2d"), keep = [], n = 0;
function frame() {
    keep.push(new Float64Array(65536 + (n % 7) * 4096));
    if (keep.length > 64) { keep.splice(0, 32); }
    c.fillStyle = "hsl(" + (n++ % 360) + ",80%,50%)";
    c.fillRect(Math.random() * 760, Math.random() * 560, 40, 40);
    requestAnimationFrame(frame);
}
frame();
</script></body></html>
'@
    $edge = @("${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe",
              "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe") |
            Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $edge) {
        Write-Result 'browser.output=msedge.exe not found'
        return
    }

    $arguments = "--no-first-run --no-default-browser-check --user-data-dir=`"$labDir\edge`" `"$page`""
    $action = New-ScheduledTaskAction -Execute $edge -Argument $arguments
    $principal = New-ScheduledTaskPrincipal -UserId "$env:COMPUTERNAME\Administrator" -LogonType Interactive
    Register-ScheduledTask -TaskName 'ShvLabBrowser' -Action $action -Principal $principal -Force | Out-Null
    Start-ScheduledTask -TaskName 'ShvLabBrowser'
}

function Stop-Browser {
    Get-Process -Name 'msedge' -ErrorAction SilentlyContinue | Stop-Process -Force
    Unregister-ScheduledTask -TaskName 'ShvLabBrowser' -Confirm:$false -ErrorAction SilentlyContinue
    for ($i = 0; ($i -lt 20) -and ((Get-BrowserCount) -ne 0); $i++) {
        Start-Sleep -Milliseconds 500
    }
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
sc.exe create $service type= kernel binPath= (Join-Path $labDir 'euler.sys') | Out-Null

Invoke-Phase 'before'

#
# Loading virtualizes every processor. If SimpleVisor halts, this never returns,
# and the host notices through its timeout.
#
$output = sc.exe start $service 2>&1
Write-Result "load.exit=$LASTEXITCODE"
Write-Result "load.output=$(($output | Where-Object { $_ }) -join ' | ')"

#
# Run a web browser while SimpleVisor is loaded, and keep it running across
# the unload. The OS then goes on with the mappings it cached, which a stale
# TLB would show as a crash or a hang
#
Start-Browser
Start-Sleep -Seconds 15
Write-Result "browser.loaded=$(Get-BrowserCount)"

Invoke-Phase 'loaded'

$output = sc.exe stop $service 2>&1
Write-Result "unload.exit=$LASTEXITCODE"
Write-Result "unload.output=$(($output | Where-Object { $_ }) -join ' | ')"

Start-Sleep -Seconds 15
Write-Result "browser.after=$(Get-BrowserCount)"

Invoke-Phase 'after'

Stop-Browser
Write-Result "browser.stopped=$(Get-BrowserCount)"

sc.exe delete $service | Out-Null
Write-Result 'done=1'
