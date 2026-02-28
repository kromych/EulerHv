<#
.SYNOPSIS
    Builds the files that Invoke-ShvTest.ps1 deploys to the lab VM.

.DESCRIPTION
    Using the Enterprise WDK, builds SimpleVisor (NT configuration) and the
    shvcheck.exe guest probe, and test-signs shv.sys with a self-signed lab
    certificate, created on first use in the current user's certificate store.

    Everything goes to vmlab\out: shv.sys, shv.pdb, shvcheck.exe and
    SimpleVisorLab.cer.

.PARAMETER WdkRoot
    The root of the Enterprise WDK, i.e. the directory with LaunchBuildEnv.cmd.
    Defaults to $env:SHV_WDK_ROOT, or to a "wdk" directory next to the repo.
#>
[CmdletBinding()]
param (
    [string] $WdkRoot,
    [string] $OutDir
)

. (Join-Path $PSScriptRoot 'LabCommon.ps1')

if (-not $WdkRoot) { $WdkRoot = $LabDefaults.WdkRoot }
if (-not $OutDir) { $OutDir = $LabDefaults.OutDir }

$setupEnv = Join-Path $WdkRoot 'BuildEnv\SetupBuildEnv.cmd'
if (-not (Test-Path $setupEnv)) {
    throw "The Enterprise WDK was not found at $WdkRoot. Pass -WdkRoot, or set SHV_WDK_ROOT."
}
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

#
# Build both binaries from one EWDK command prompt. The EWDK environment only
# covers the kernel-mode build, so add the SDK's user-mode headers and libraries
# for shvcheck.exe. Test signing is done separately below, as the project's own
# signing step fails with the current signtool.
#
$script = @"
@echo off
call "$setupEnv" amd64 >nul 2>&1
msbuild "$RepoRoot\shv.vcxproj" /nologo /v:minimal /p:Configuration=NT /p:Platform=x64 /p:SignMode=Off || exit /b 1
set "INCLUDE=%INCLUDE%;%WindowsSdkDir%Include\%Version_Number%\ucrt;%WindowsSdkDir%Include\%Version_Number%\um;%WindowsSdkDir%Include\%Version_Number%\shared"
set "LIB=%LIB%;%WindowsSdkDir%Lib\%Version_Number%\ucrt\x64;%WindowsSdkDir%Lib\%Version_Number%\um\x64"
cl /nologo /W4 /WX /O2 /MT "$PSScriptRoot\guest\shvcheck.c" /Fe:"$OutDir\shvcheck.exe" /Fo:"$OutDir\\" || exit /b 1
"@
$scriptPath = Join-Path ([System.IO.Path]::GetTempPath()) "shvlab-build-$PID.cmd"
[System.IO.File]::WriteAllText($scriptPath, $script)
try {
    Write-Host 'Building SimpleVisor and shvcheck.exe...'
    $build = Invoke-NativeCommand -FilePath $env:ComSpec -Arguments @('/c', $scriptPath) -AllowFailure
    if ($build.ExitCode -ne 0) {
        throw "The build failed:`n$($build.Output)"
    }
}
finally {
    Remove-Item -Path $scriptPath -ErrorAction SilentlyContinue
}
Remove-Item -Path (Join-Path $OutDir 'shvcheck.obj') -ErrorAction SilentlyContinue
Copy-Item -Path (Join-Path $RepoRoot 'x64\NT\shv.sys'), (Join-Path $RepoRoot 'x64\NT\shv.pdb') -Destination $OutDir

#
# Test-sign the driver with the lab certificate
#
$subject = 'CN=SimpleVisor Lab Test Signing'
$cert = Get-ChildItem -Path Cert:\CurrentUser\My -CodeSigningCert |
        Where-Object { ($_.Subject -eq $subject) -and ($_.NotAfter -gt (Get-Date)) } |
        Select-Object -First 1
if (-not $cert) {
    Write-Host 'Creating the lab test-signing certificate...'
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject `
                                      -CertStoreLocation Cert:\CurrentUser\My `
                                      -NotAfter (Get-Date).AddYears(5)
}
Export-Certificate -Cert $cert -FilePath (Join-Path $OutDir 'SimpleVisorLab.cer') | Out-Null

$signtool = Get-ChildItem -Path (Join-Path $WdkRoot 'Program Files\Windows Kits\10\bin\*\x64\signtool.exe') |
            Sort-Object FullName | Select-Object -Last 1
if (-not $signtool) {
    throw 'signtool.exe was not found in the Enterprise WDK.'
}
Invoke-NativeCommand -FilePath $signtool.FullName `
                     -Arguments @('sign', '/q', '/fd', 'SHA256', '/s', 'My', '/sha1', $cert.Thumbprint, (Join-Path $OutDir 'shv.sys')) | Out-Null

Write-Host "Built and signed the lab artifacts in $OutDir"
