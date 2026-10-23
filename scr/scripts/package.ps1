[CmdletBinding()]
param(
    [string]$OutputDirectory,
    [string]$WorkDirectory,
    [string]$InnoCompiler,
    [string]$VisualStudioPath
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$releaseRoot = Split-Path -Parent $projectRoot
if (!$OutputDirectory) { $OutputDirectory = Join-Path $releaseRoot 'setup' }
if (!$WorkDirectory) { $WorkDirectory = Join-Path $projectRoot 'build\packaging' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$WorkDirectory = [IO.Path]::GetFullPath($WorkDirectory)
$payload = Join-Path $WorkDirectory 'payload'
if (!$InnoCompiler) {
    $candidates = @(
        (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
        (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')
    )
    $InnoCompiler = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (!$InnoCompiler) { $InnoCompiler = (Get-Command ISCC.exe -ErrorAction SilentlyContinue).Source }
}
if (!$InnoCompiler -or !(Test-Path -LiteralPath $InnoCompiler)) { throw 'Install Inno Setup 6.5+ and pass -InnoCompiler <path to ISCC.exe>.' }
New-Item -ItemType Directory -Force -Path $OutputDirectory,$WorkDirectory,$payload | Out-Null
& (Join-Path $PSScriptRoot 'build.ps1') -Configuration release -BuildDirectory (Join-Path $WorkDirectory 'build') -InstallDirectory $payload -VisualStudioPath $VisualStudioPath -WithInstaller -Install
& $InnoCompiler "/DPayloadDir=$payload" "/DOutputDir=$OutputDirectory" (Join-Path $projectRoot 'packaging\DesktopBall.iss')
if ($LASTEXITCODE -ne 0) { throw 'Installer compilation failed.' }
$installer = Join-Path $OutputDirectory 'TaiJiAura-1.0-x64-Setup.exe'
$hash = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText("$installer.sha256", "$hash  $([IO.Path]::GetFileName($installer))`n", [Text.UTF8Encoding]::new($false))
Write-Output "Installer: $installer"
