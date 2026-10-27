@echo off
rem Entfernt den Shadow Man Co-op Mod und stellt das Original wieder her.
rem Meldungen auf Deutsch, wenn Windows deutsch ist, sonst Englisch (Test: set SMCOOP_LANG=en).
setlocal
cd /d "%~dp0"
set "SMLANG=%SMCOOP_LANG%"
if not defined SMLANG for /f "usebackq delims=" %%a in (`powershell -NoProfile -Command "(Get-UICulture).TwoLetterISOLanguageName" 2^>nul`) do set "SMLANG=%%a"
set "DE="
if /i "%SMLANG%"=="de" set "DE=1"

if not exist xaudio2_9redist_orig.dll (
    if defined DE (echo Der Mod ist nicht installiert.) else (echo The mod is not installed.)
    pause
    exit /b 0
)
copy /y xaudio2_9redist_orig.dll xaudio2_9redist.dll >nul
del xaudio2_9redist_orig.dll
del smcoop_net.txt smcoop_log.txt smcoop_log_prev.txt smcoop_ghost.txt smcoop_watch.txt 2>nul
del smcoop_dump_*.bin 2>nul
if defined DE (echo Mod entfernt, Original wiederhergestellt.) else (echo Mod removed, original restored.)
pause
