Work-in-progress source kit (pre-release): no game, no XEX, no generated code, no game executable; `Setup.exe` is our installer and launcher. Bring your own Xbox 360 Brazil (En,Pt) disc.

**v0.2.5**

- **`Setup.exe`**: download it alone and run it. It fetches this repository's source, shows on its *Dependencies* tab what your PC has and what is missing (Python, C++ Build Tools, LLVM/CMake/Ninja, your game files, the built game) with the space each part needs, installs each part or everything, and builds the game. Afterwards it is the launcher: *Play*, and *PC settings* with the same options as the F1 menu, saved before the game starts. Dark and light themes (the default follows Windows). It is built by GitHub from `tools/launcher`; nothing from the game is in it. Windows may warn because it is not signed.
- `Setup.cmd` still works as before; `tools/setup.ps1` gained `-Only python|buildtools|tools|game`.

`Setup.exe` was tested with the FIFA Street 3 kit (stand-in install steps, Play, settings file, real kit download up to the build tools); for this game only its window and settings were checked, not a build through it.

**v0.2.4**

- **One-click `Setup.cmd`:** asks before installing anything, installs Python and Microsoft's C++ Build Tools through winget if missing, downloads pinned, SHA-256-checked LLVM/Clang, CMake and Ninja into `.local/tools` only, fetches missing parts, opens the game-file installer and builds the port; `Play-<game>.cmd` starts it. Run it again after a failure: finished steps are skipped.
- **README:** a *How to play* guide for people new to this, and the same section layout as the other ports.
- The source-kit ZIP now includes the game-file installer (`tools/port-installer`); `Install-Files.cmd` did not work from the ZIP before.

`Setup.cmd` has not been tried from a fresh download with this game yet (only with FIFA Street 3). Please report problems with `setup-log.txt`.

**v0.2.3**

- **Controller vibration** on/off in the F1 PC settings.
- The game-data installer launcher is now `Install-Files.cmd` (English name).
- The shared game-data installer (`tools/port-installer`) ships in the repository.

Vibration has been compiled but not felt on a real controller yet. Nobody has played this exact build through.

**v0.2.2**

- The game's own audio plays now: a full XMA ring was read as empty, so voices timed out and played silence. The level is low (about -35 dB RMS) and nobody has reviewed it by ear.
- At 4K, 16x anisotropic filtering, no frame limit, in a Skill Game (Ryzen 5 3600, RTX 3050 Laptop), without MSAA / 2x / 4x / 8x: 142/79/72/54 fps before, 168/114/101/75 fps now. Small post-effect targets no longer resolve the whole 4K surface, and copies of render results to the game's memory wait until the CPU needs them.
- SIMD shader-constant copy, write-watched texture, vertex and index caches.
- PC settings menu reads every SDL pad; controller mappings sit next to the executable.
- The game keeps running when the audio device stops consuming.
- Experimental macOS build files (not compiled or run yet) and a native Linux build path (not built for this release).

Nobody has played this exact build through.

**Earlier**

- **Now a full build kit:** `tools/build_local.py` downloads the pinned ReXGlue SDK, translates the game's three modules locally and builds the port; `tools/play.py` runs it.
- **Keyboard play:** the keyboard plays as an Xbox controller with EA SPORTS FC 27's default WASD layout (WASD move, L pass, ; shoot, K lob/cross, O through ball, P sprint, I change player, Esc pause; Enter passes PRESS START).
- Native Direct3D 11 renderer and native XMA audio; no GPU or audio-hardware emulation.
- Reaches menus, Kick Off matches and the Skill Games on Windows 11 x64; crashes and occasional one-frame glitches may remain.
- **F1** opens the PC settings over the game (display, resolution, VSync, FXAA, language, volume).
- `tools/extract_iso.py` + `tools/validate_game.py`: extract your disc and verify the three executables by SHA-256.
