Work-in-progress source kit (pre-release): no game, no XEX, no generated code and no executable. Bring your own Xbox 360 Brazil (En,Pt) disc.

- **Now a full build kit:** `tools/build_local.py` downloads the pinned ReXGlue SDK, translates the game's three modules locally and builds the port; `tools/play.py` runs it.
- **Keyboard play:** the keyboard plays as an Xbox controller with EA SPORTS FC 27's default WASD layout (WASD move, L pass, ; shoot, K lob/cross, O through ball, P sprint, I change player, Esc pause; Enter passes PRESS START).
- Native Direct3D 11 renderer and native XMA audio; no GPU or audio-hardware emulation.
- Reaches menus, Kick Off matches and the Skill Games on Windows 11 x64; crashes and occasional one-frame glitches may remain.
- **F1** opens the PC settings over the game (display, resolution, VSync, FXAA, language, volume).
- `tools/extract_iso.py` + `tools/validate_game.py`: extract your disc and verify the three executables by SHA-256.
