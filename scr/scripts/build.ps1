[CmdletBinding()]
param(
    [ValidateSet('debug','release')][string]$Configuration = 'release',
    [string]$BuildDirectory,
    [string]$InstallDirectory,
    [string]$VisualStudioPath,
    [switch]$WithInstaller,
    [switch]$Install
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (!$BuildDirectory) { $BuildDirectory = Join-Path $projectRoot "build\$Configuration" }
if (!$InstallDirectory) { $InstallDirectory = Join-Path $projectRoot 'dist' }
if (!$VisualStudioPath) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $VisualStudioPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    }
}
if ($VisualStudioPath) {
    Import-Module (Join-Path $VisualStudioPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $VisualStudioPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
    $bundled = Join-Path $VisualStudioPath 'Common7\IDE\CommonExtensions\Microsoft\CMake'
    foreach ($toolDirectory in @((Join-Path $bundled 'CMake\bin'),(Join-Path $bundled 'Ninja'))) {
        if (Test-Path -LiteralPath $toolDirectory) { $env:PATH = $toolDirectory + ';' + $env:PATH }
    }
}
foreach ($tool in @('cl.exe','cmake.exe','ninja.exe')) {
    if (!(Get-Command $tool -ErrorAction SilentlyContinue)) { throw "Missing $tool. Use an x64 Native Tools terminal or pass -VisualStudioPath for Visual Studio with Desktop C++ and CMake/Ninja tools." }
}
if ($env:VSCMD_ARG_TGT_ARCH -and $env:VSCMD_ARG_TGT_ARCH -ne 'x64') { throw 'Use an x64 compiler environment.' }
$buildType = if ($Configuration -eq 'debug') { 'Debug' } else { 'Release' }
$helper = if ($WithInstaller) { 'ON' } else { 'OFF' }
& cmake.exe -S $projectRoot -B $BuildDirectory -G Ninja "-DCMAKE_BUILD_TYPE=$buildType" "-DCMAKE_INSTALL_PREFIX=$InstallDirectory" "-DDESKTOPBALL_BUILD_UNINSTALL_LAUNCHER=$helper"
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& cmake.exe --build $BuildDirectory
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
if ($Install) {
    & cmake.exe --install $BuildDirectory
    if ($LASTEXITCODE -ne 0) { throw 'Staging failed.' }
}
