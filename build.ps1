# Baut src\ -> build\xaudio2_9redist.dll mit den Visual Studio Build Tools (MSVC).
# -Dev: Entwickler-Build mit Analysetasten F6/F8/F9/F11 (SMCOOP_DEV) - nicht fuer Releases
param([switch]$Dev)
$ErrorActionPreference = 'Stop'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "Visual Studio Build Tools nicht gefunden. Bitte zuerst installieren." }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "C++-Werkzeuge (MSVC) fehlen in der Visual-Studio-Installation." }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

New-Item -ItemType Directory -Force "$PSScriptRoot\build" | Out-Null
# vcvars64.bat ruft selbst vswhere.exe auf; ohne den Installer-Ordner im PATH meldet es einen (harmlosen) Fehler
$env:PATH = "$(Split-Path $vswhere);$env:PATH"
$devFlag = if ($Dev) { '/DSMCOOP_DEV ' } else { '' }
$cmd = "`"$vcvars`" >nul && cd /d `"$PSScriptRoot`" && " +
       "rc /nologo /fo build\smcoop.res src\smcoop.rc && " +
       "cl /nologo /O2 /EHsc /std:c++17 /MT /W3 /LD $devFlag" + "src\dllmain.cpp src\net.cpp src\overlay.cpp build\smcoop.res " +
       "/Fo:build\ /Fe:build\xaudio2_9redist.dll"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "Build fehlgeschlagen (Exit $LASTEXITCODE)." }
Write-Host "Fertig: build\xaudio2_9redist.dll"
