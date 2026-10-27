# Baut und startet die Logik-Tests (tests\test_logic.cpp -> build\test_logic.exe). Exit 0 = alle bestanden.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
New-Item -ItemType Directory -Force "$root\build\tests" | Out-Null
$env:PATH = "$(Split-Path $vswhere);$env:PATH"
cmd /c "`"$vcvars`" >nul && cd /d `"$root`" && cl /nologo /EHsc /std:c++17 /W3 tests\test_logic.cpp /Fo:build\tests\ /Fe:build\tests\test_logic.exe >build\tests\compile.txt"
if ($LASTEXITCODE -ne 0) { Get-Content "$root\build\tests\compile.txt" | Select-Object -Last 20; throw "Test-Build fehlgeschlagen" }
& "$root\build\tests\test_logic.exe"
exit $LASTEXITCODE
