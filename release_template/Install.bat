@echo off
rem Installiert den Shadow Man Co-op Mod (muss im Shadow-Man-Ordner neben thoth_x64.exe liegen).
rem Meldungen auf Deutsch, wenn Windows deutsch ist, sonst Englisch (Test: set SMCOOP_LANG=en).
setlocal
cd /d "%~dp0"
set "SMLANG=%SMCOOP_LANG%"
if not defined SMLANG for /f "usebackq delims=" %%a in (`powershell -NoProfile -Command "(Get-UICulture).TwoLetterISOLanguageName" 2^>nul`) do set "SMLANG=%%a"
set "DE="
if /i "%SMLANG%"=="de" set "DE=1"

if not exist thoth_x64.exe (
    if defined DE (
        echo FEHLER: Bitte den Inhalt der ZIP-Datei in den Shadow-Man-Ordner kopieren
        echo         ^(Steam: Rechtsklick auf Shadow Man - Verwalten - Lokale Dateien durchsuchen^)
    ) else (
        echo ERROR: Please copy the content of the ZIP file into the Shadow Man folder
        echo        ^(Steam: right-click Shadow Man - Manage - Browse local files^)
    )
    pause
    exit /b 1
)
if not exist SMCoop\xaudio2_9redist.dll (
    if defined DE (echo FEHLER: Ordner SMCoop fehlt) else (echo ERROR: the SMCoop folder is missing)
    pause
    exit /b 1
)
rem Original nur beim ersten Mal sichern
if not exist xaudio2_9redist_orig.dll copy /y xaudio2_9redist.dll xaudio2_9redist_orig.dll >nul
copy /y SMCoop\xaudio2_9redist.dll xaudio2_9redist.dll >nul

rem Mitspieler kommen ueber die Koop-Lobby im Hauptmenue
echo lobby> smcoop_net.txt
echo.
if defined DE (
    echo Fertig! Der Mod ist installiert. Shadow Man ueber Steam starten.
    echo Host: Hauptmenue - Koop-Lobby - Lobby erstellen - Freunde einladen.
    echo Gast: Steam-Einladung annehmen und im Hauptmenue warten.
) else (
    echo Done! The mod is installed. Start Shadow Man via Steam.
    echo Host: main menu - Co-op Lobby - Create lobby - Invite friends.
    echo Guest: accept the Steam invite and wait in the main menu.
)
pause
