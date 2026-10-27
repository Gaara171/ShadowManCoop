# Shadow Man Co-op (SMCoop)

Co-op multiplayer mod for **Shadow Man Remastered** (Steam): up to four players
over Steam, with shared enemies, bosses, progress, doors, puzzles, pickups and
cutscenes.

Download and player instructions:
https://www.nexusmods.com/shadowmanremastered/mods/21

This repository holds the complete source of the mod DLL, so anyone can check
what it does and build it themselves. Code comments are in German.

## How it works

- The mod is built as `xaudio2_9redist.dll`, a proxy for the XAudio2 DLL that
  the game loads from its own folder. All six exports are forwarded to the
  original DLL, which `Install.bat` keeps as `xaudio2_9redist_orig.dll`
  (see the `/EXPORT:` lines at the top of `src/dllmain.cpp`).
- When loaded, it hooks functions of the game (`thoth_x64.exe`) inside the game
  process to show the other players and to keep enemies, objects and progress
  in sync.
- Networking only uses the Steam API that the game already loads
  (`steam_api64.dll`): Steam lobby, invites and Steam P2P messages
  (`src/net.cpp`). A plain UDP mode exists only for local tests with several
  game instances on one PC.
- Name tags are drawn with the game's own Direct3D 11 device (`src/overlay.cpp`).
- The mod downloads nothing. It writes its log and settings (`smcoop_*.txt`)
  into the game folder. Only in Hardcore mode does it delete the saves of that
  Hardcore run in the game's save folder, as the mode promises.

## Files

| Path | Content |
| --- | --- |
| `src/dllmain.cpp` | Hooks, player figures, sync of enemies/objects/progress, menus |
| `src/net.cpp`, `src/net.h` | Steam lobby and P2P, UDP test mode |
| `src/overlay.cpp`, `src/overlay.h` | Name tag overlay |
| `src/coop_logic.h`, `src/pose_codec.h` | Pure logic and network message formats (unit tested) |
| `src/i18n.h` | German/English texts |
| `src/smcoop.rc` | Version info of the DLL |
| `tests/` | Unit tests for the pure logic |
| `release_template/` | `Install.bat`, `Uninstall.bat` and readme files of the download |

## Build

Requirements: Windows and the Visual Studio Build Tools with the C++ workload
(MSVC x64).

```
powershell -ExecutionPolicy Bypass -File build.ps1
```

Result: `build\xaudio2_9redist.dll`. The switch `-Dev` adds debug hotkeys;
release builds are made without it.

Tests:

```
powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1
```

`TestFontTable` reads a font from the game archive `ShadowManEX01.kpf`; adjust
its path in `tests/test_logic.cpp` to your game folder.

## Package

The download is the content of `release_template/` plus a folder `SMCoop`
that contains the built `xaudio2_9redist.dll`.

## Permissions

Free to use, modify and re-upload with credit to Gaara.
