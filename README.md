# 2014 FIFA World Cup Brazil — Native PC Port (early work, native components only)

> [!WARNING]
> This repository does **not** include the game, an ISO/XEX, EA assets, generated game code or a precompiled executable.
> It is **not playable** yet. See [the full notice](#notice) at the end.

Early work towards a native PC port of the Xbox 360 game *2014 FIFA World Cup Brazil* (Copa do Mundo da FIFA Brasil 2014), based on static recompilation with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).

## Why only part of the project is here

A private build of this game boots to the title screen and team selection, but its graphics still go through an **emulated** Xbox 360 GPU (the SDK's Xenos plugin) and its audio through emulated XMA contexts. This repository only publishes the parts that are already native and free of game content:

| Path | What it is |
| --- | --- |
| `project/src/shader_metadata.h`, `source_shader_asset.h`, `shader_parameter_updates.h`, `source_vertex_layout.h` | Standalone importers for the game's shader and vertex-layout metadata into the PC's own buffers, with bounds, endianness and count checks. They are the groundwork for a native renderer that does not yet exist. |
| `project/src/pc_menu.*`, `pc_audio.*` | In-game PC settings menu and per-process volume control (Windows audio session API). |
| `project/src/kernel_extra.cpp`, `perf_hooks.cpp`, `script_input.h` | Missing kernel exports (camera reported as not connected), frame counter, scripted controller for automated tests. |
| `project/copa2014_manifest.toml` | Function boundaries the translator needs (addresses and sizes only). |
| `tests/` | Game-free tests of the importers. |
| `tools/` | Disc extraction, file validation and the publication audit. |

The application wiring, build files and GPU/audio paths that depend on emulation are deliberately **not** published. There is no build script for the game yet: one will be added when the renderer and audio are native, as in the [FIFA Street 3](https://github.com/batatinhafcbatatinha-pixel/fifa-street-3-pc) and [Battlefield 2: Modern Combat](https://github.com/batatinhafcbatatinha-pixel/bf2-modern-combat-pc) ports.

## Why a native port instead of an emulator

An emulator such as [Xenia](https://xenia.jp) recreates the Xbox 360 hardware and is the right tool for the whole library. The goal here is narrower: ahead-of-time compiled game code, draw calls going straight to the PC's graphics API and audio decoded natively, with every system readable and fixable in plain C++. That goal is not reached for this game yet.

## Your game files

Only the **Xbox 360 Brazil (En,Pt)** revision is targeted. You can already extract and validate your own disc:

```sh
python tools/extract_iso.py "Copa do Mundo da FIFA Brasil 2014 (Brazil) (En,Pt).iso" --output game_files
python tools/validate_game.py game_files
```

```text
default.xex                                                         63e6ebaf5117dbfaa67cb43b033486a3aeb1b1b2f644dcf61b8482e4d06c14bd
dlc/dlc_powdll/dlc/powdll/powdllzf.xex.dll                          8ec0b3e5ef644b1c27e02c1b1111ad1aa93ee4bfee7653658f6931a949d17d80
dlc/dlc_FootballCompEng/dlc/FootballCompEng/FootballCompEngzf.xex.dll  7888f6c73e6706f7a7a401f8247f9bd95deca7edcaa2c0f5940281449a27f7d6
```

## Run the tests

```sh
python tools/run_native_tests.py          # clang++ or g++ with C++20
python tools/test_tools.py -v             # extractor and validator, no game needed
```

## Credits and licenses

- Port work: [batatinhafcbatatinha-pixel](https://github.com/batatinhafcbatatinha-pixel).
- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk): Tom Clay and contributors, BSD-3-Clause; derived from [Xenia](https://github.com/xenia-project/xenia). Not included; license in `licenses/`.
- 2014 FIFA World Cup Brazil, its game code, trademarks and assets belong to Electronic Arts, FIFA and the respective rights holders. This project is not affiliated with or endorsed by EA, FIFA or Microsoft.

See [NOTICE.md](NOTICE.md) and [LICENSE](LICENSE).

---

<a id="notice"></a>

**⚠️ THIS PROJECT DOES NOT INCLUDE THE GAME, AN ISO/XEX, EA GAME ASSETS, GENERATED GAME CODE OR A PRECOMPILED GAME EXECUTABLE. YOU MUST PROVIDE YOUR OWN LAWFULLY OBTAINED XBOX 360 GAME FILES AND HAVE THE RIGHTS REQUIRED FOR YOUR USE. THIS PROJECT DOES NOT AUTHORIZE COPYRIGHT INFRINGEMENT. ALL GAME RIGHTS BELONG TO THEIR RESPECTIVE OWNERS. ⚠️**
