# Windows build entry point -- the counterpart to build.sh for the windows-*
# presets. Enters the x64 VS dev shell (the presets need the Windows SDK
# INCLUDE/LIB, which CMake presets cannot synthesize) and drives the preset.
# Invoked by `build.sh --target windows` on the Windows build VM.
[CmdletBinding()]
param(
    [switch]$Release,
    [switch]$Clean,
    [switch]$NoJoltc
)
$ErrorActionPreference = 'Stop'

# sshd caches PATH at service start; refresh from the registry so freshly
# installed tools (clang-cl, cmake, ninja) are on PATH in this session.
$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' +
            [Environment]::GetEnvironmentVariable('Path','User')

# Enter the x64 VS dev shell for the Windows SDK INCLUDE/LIB.
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio Build Tools with the C++ x64 toolset not found.' }
Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

$preset = if ($Release) { 'windows-release' } else { 'windows-debug' }
$joltc  = if ($NoJoltc) { 'OFF' } else { 'ON' }

# Run from the repo root (this script lives in <root>/tools).
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

$fresh = if ($Clean) { '--fresh' } else { $null }
Write-Host "Configuring ($preset)..."
cmake --preset $preset $fresh -D "CETRA_BUILD_JOLTC=$joltc"
if ($LASTEXITCODE -ne 0) { throw "configure failed ($LASTEXITCODE)" }

Write-Host 'Building...'
cmake --build --preset $preset
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

Write-Host "Done ($preset). Artifacts in out/$preset/bin"
