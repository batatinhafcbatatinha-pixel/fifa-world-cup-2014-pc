Work-in-progress source kit (pre-release): no game, no XEX, no generated code and no executable. Bring your own Xbox 360 Brazil (En,Pt) disc.

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
