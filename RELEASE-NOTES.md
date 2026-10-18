Early-work source drop (pre-release, not playable): native components only, no game, no XEX, no generated code and no executable.

- Native importers for the game's shader and vertex-layout metadata, with game-free C++20 tests.
- PC settings menu, volume control, missing kernel exports and test tooling.
- `tools/extract_iso.py` + `tools/validate_game.py`: extract your Xbox 360 Brazil disc and verify the three executables by SHA-256.
- Not included on purpose: everything that still depends on the emulated GPU/audio of the private build.
