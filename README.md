# 2014 FIFA World Cup Brazil — Native PC Port (work in progress) | No Emulated GPU or Audio Hardware

> [!WARNING]
> This repository does **not** include the game, an ISO/XEX, EA assets, generated game code or a precompiled executable.
> You need your own Xbox 360 Brazil (En,Pt) disc. See [the full notice](#notice) at the end.

An experimental port of the Xbox 360 game *2014 FIFA World Cup Brazil* (Copa do Mundo da FIFA Brasil 2014), built through static recompilation with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), with its own Direct3D 11 renderer and its own XMA audio pipeline. **This public repository is a source and local-build kit, not a ready-to-play game download.** The game's PowerPC code (`default.xex` and its two game libraries `powdllzf.xex.dll` and `FootballCompEngzf.xex.dll`) is translated locally from your own verified disc; no game executable or generated game code is distributed here.

## How to play

> [!IMPORTANT]
> **There is no ready-to-play download.** This page has only tools: they turn *your own* game disc into a PC game,
> on your computer. The first time takes a while (20-60 minutes on a recent PC), mostly waiting.
> After that, you just double-click to play.

**You need**

- **Windows 10 or 11** (64-bit) with about **23 GB** free. (Linux and Mac: see below.)
- Your own **2014 FIFA World Cup Brazil** (Xbox 360, Brazil (En,Pt)), as a disc image (`.iso`) made from your disc, or its extracted
  files. Other versions are not accepted; the setup checks this for you.

**Steps**

**Easiest: `Setup.exe`.** Open [Releases](https://github.com/batatinhafcbatatinha-pixel/fifa-world-cup-2014-pc/releases), download **`Setup.exe`** from the newest one and run it.
It downloads this kit (source code only), shows what your PC already has and what is missing on its
*Dependencies* tab, installs what is missing, asks for your `.iso` and builds the game. Afterwards the same
program (and its desktop shortcut) is your launcher: **Play**, and **PC settings** (the same options as F1 in
the game, before it starts). Windows may say it protected your PC because the program is not signed: click
*More info* > *Run anyway*. The program is built by GitHub from [tools/launcher](tools/launcher); nothing from the game is in it.

**Or step by step:**

1. **Download**, one of these two ways:
   <details open><summary><b>Easiest: the ZIP</b></summary>

   Open [Releases](https://github.com/batatinhafcbatatinha-pixel/fifa-world-cup-2014-pc/releases), download from the newest one **`fifa-world-cup-2014-pc-source-kit.zip`** and extract it to a short folder, such as
   `C:\Games\fifa-world-cup-2014-pc`.
   </details>
   <details><summary><b>With Git: always the newest version</b></summary>

   ```powershell
   git clone --recursive https://github.com/batatinhafcbatatinha-pixel/fifa-world-cup-2014-pc.git C:\Games\fifa-world-cup-2014-pc
   ```
   To update later: `git pull --recurse-submodules`, then run `Setup.cmd` again.
   </details>

2. **Double-click `Setup.cmd`.** It asks before installing anything, then:
   - installs Python and Microsoft's C++ Build Tools if you don't have them (free; Windows asks for permission once);
   - downloads the other build tools (about 1 GB) **into this folder only**;
   - opens a window where you **pick your `.iso`**: it checks the version and copies the game files;
   - builds your PC version, and offers a desktop shortcut.

   If anything stops, run `Setup.cmd` again: finished steps are skipped.

3. **Play:** double-click **`Play-FIFA-World-Cup-2014.cmd`** (or the desktop shortcut).

**In the game:** **F1** opens the PC settings. Controllers and the keyboard work: see [Controls](#controls).

**Problems?** Open an [issue](https://github.com/batatinhafcbatatinha-pixel/fifa-world-cup-2014-pc/issues) with `setup-log.txt` and a screenshot. Never attach game files.

`Setup.cmd` is new: so far it has been tried from a fresh download only with the FIFA Street 3 port. If it stops for you, please open an issue with `setup-log.txt`.

<details><summary><b>Linux and Mac</b></summary>

- **Linux:** works natively (no Wine), but the setup is by hand for now: see [Linux](#linux).
- **Mac:** not playable yet. The Mac build files are an experiment that has never been compiled or run on a Mac. Details in [MACOS.md](MACOS.md); if you have a Mac and want to help test, open an issue.

</details>

<details><summary><b>Questions</b></summary>

- **Is this legal?** This page contains no game, only tools. You use the game you already own.
- **Can I share my built game?** No: it contains the game's code. Keep it on your PC.
- **Will it run on my PC?** Any graphics card with DirectX 11 should work. Measured: about 168 fps in a Skill Game at 4K on a Ryzen 5 3600 + RTX 3050 Laptop.
- **How do I remove it?** Delete the folder. Python and the Build Tools can be removed in Windows Settings > Apps.

</details>

## Status

- Reached on Windows 11 x64 with the native renderer: boot, menus, Kick Off (team, kit and stadium selection) and full matches, and the Skill Games.
- Graphics: the game's Direct3D library is hooked at the API level and drawn by a native Direct3D 11 renderer; Xenos shader microcode is translated to HLSL SM5; textures are untiled and converted on load. Render-to-texture results the CPU reads (crowd sprite atlases, generated lightmaps) are written back to game memory when the game touches them. No GPU command processor or emulation plugin.
- Audio: XMA contexts are decoded with FFmpeg's XMA decoder and mixed natively. No audio hardware model. The game's own audio only started playing with a decoder fix on 2026-11-01: a loopback capture of menus and a Skill Game has no silent stretches, but the level is low (about -35 dB RMS against -17 to -28 dB in the other ports) and nobody has reviewed it by ear.
- Performance in a Skill Game at 4K with 16x anisotropic filtering and no frame limit (Ryzen 5 3600, RTX 3050 Laptop): about 168 fps without MSAA, 114 with 2x, 101 with 4x, 75 with 8x.
- The Xbox 360 kernel and XAM services come from the ReXGlue runtime as an operating-system compatibility layer. Online features are not supported.
- Known issues: crashes and occasional one-frame graphical glitches may remain. Tested on Windows x64; a Linux build path exists. Experimental [macOS](MACOS.md) build files are included; they have not been compiled or run yet.

## Controls

### Controller

Xbox pads and every pad SDL knows work in the game and in the PC settings menu: DualShock 3/4, DualSense, Switch pads, USB adapters for PS2 and original Xbox pads. `tools/build_local.py` downloads `gamecontrollerdb.txt` (SDL_GameControllerDB, zlib license) next to the executable, so generic adapters get their button names. Only Xbox-type input was tested; no PlayStation pad or adapter has been tried on real hardware yet. The pad vibrates when the game asks for it; **Controller vibration** in the PC settings turns that off.

### Keyboard and mouse

No controller needed: the keyboard plays as an Xbox controller, with the keys of the default **WASD** layout of EA SPORTS FC 27 ([EA's layout chart](https://help.ea.com/en/articles/ea-sports-fc/mouse-keyboard/)). Only the basic actions are mapped for now.

| Key | In a match | In menus | Controller button |
| --- | --- | --- | --- |
| **W A S D** (or arrows) | Move the player | Navigate | Left stick |
| **L** | Ground pass · press (defending) | Confirm | A |
| **;** (**Ç** on ABNT2 keyboards) | Shoot · tackle (defending) | Back | B |
| **K** | Lob pass / cross · sliding tackle | | X |
| **O** | Through pass · goalkeeper rush | | Y |
| **P** | Sprint | | RT |
| **I** | Change player | | LB |
| **Enter** | | Confirm, passes *PRESS START* | A + Start |
| **Esc** | Pause | | Start |
| **Backspace** | | Back | B |
| **Tab** | | | Back |

The keyboard and an Xbox controller work at the same time. Skill moves, modifier keys (finesse, chip, power shots) and the mouse are not mapped yet. To turn the keyboard off, add `teclado=0` to the `[opcoes]` section of `copa2014-pc.ini` next to the executable.

### PC settings

**F1** opens the PC settings over the game (display mode, internal resolution, VSync, frame limit, FXAA, MSAA (off, 2x, 4x or 8x; applies at once), anisotropic filtering, game language — English or Brazilian Portuguese —, volume); **Alt + Enter** or **F11** toggles fullscreen. Settings are saved in `copa2014-pc.ini` and saves in `userdata/` next to the executable. **Graphics API**: DirectX 11, or Vulkan through DXVK (restart); if something looks wrong or stutters, try the other one. **Frame rate**: 60 fps as on the console, the monitor's refresh rate (smoother on 120/144 Hz screens), no limit, or 30 fps; this game keeps real time, so it plays at the normal speed at any rate. The home screen's header line (START, "SIGN IN TO EA SERVERS" on the console, an online service the PC does not have) reads **PC SETTINGS** / **CONFIGURAÇÕES DO PC** and opens the same menu. `menu_no_jogo=0` in the ini restores the original line.  Xbox controllers work as on the console.

## Your game files

Only the **Xbox 360 Brazil (En,Pt)** revision is accepted. Extract your own disc image into a new folder and validate it:

```sh
python tools/extract_iso.py "Copa do Mundo da FIFA Brasil 2014 (Brazil) (En,Pt).iso" --output game_files
python tools/validate_game.py game_files
```

```text
default.xex                                                            63e6ebaf5117dbfaa67cb43b033486a3aeb1b1b2f644dcf61b8482e4d06c14bd
dlc/dlc_powdll/dlc/powdll/powdllzf.xex.dll                             8ec0b3e5ef644b1c27e02c1b1111ad1aa93ee4bfee7653658f6931a949d17d80
dlc/dlc_FootballCompEng/dlc/FootballCompEng/FootballCompEngzf.xex.dll  7888f6c73e6706f7a7a401f8247f9bd95deca7edcaa2c0f5940281449a27f7d6
```

Do not upload the ISO, extracted files, generated code or the built executables.

## Building by hand

`Setup.cmd` ([How to play](#how-to-play)) does all of this on Windows. The steps below are for doing it yourself, or on Linux.

### Windows

Requirements: Python 3.10+, Visual Studio 2022 Build Tools (C++ workload, Windows SDK), LLVM/Clang 20 or newer (a complete install that includes the clang resource headers), CMake 3.25+ and Ninja.

```powershell
python tools/build_local.py --game-files game_files --llvm-dir "C:\Program Files\LLVM"
python tools/play.py --game-files game_files
```

`build_local.py` validates your files, downloads the pinned ReXGlue SDK nightly (`0.10.0.24-dev.gbd833a2`, SHA-256 checked) and applies the FPSCR patch in `patches/`, downloads the FFmpeg headers matching the SDK's `libavcodec`, translates the three modules into private C++ under `.local/`, compiles the port and removes any GPU emulation plugin from the output. Expect a long first build and several GB of disk space in `.local/`; `--reuse-generated` skips the translation on later builds.

### Linux

The same kit builds a native Linux x86-64 executable, without Wine: the ReXGlue SDK's Linux package, the port's
own renderer on Direct3D 11 through DXVK native (a Linux library implementing D3D11 on Vulkan), HLSL shaders
compiled by vkd3d-shader, window and input through the SDK's SDL3. Requirements: Python 3.10+, Clang 20
(`clang-20`/`clang++-20` are picked up), CMake 3.25+, Ninja, pkg-config, fontconfig and FreeType development
files, a Vulkan driver, [DXVK native 3.1.1](https://github.com/doitsujin/dxvk/releases) and vkd3d-shader 1.19 or
newer.

```sh
python3 tools/build_local.py --game-files game_files \
  --dxvk-native-dir /path/to/dxvk-native/usr --vkd3d-dir /path/to/vkd3d-install
python3 tools/play.py --game-files game_files
```

The build copies the SDK runtime, DXVK native and vkd3d-shader next to the executable (it finds them there).

### macOS

Experimental and not compiled or run yet: see [MACOS.md](MACOS.md).

## For developers

```sh
python tools/run_native_tests.py          # shader / vertex-layout importers, clang++ or g++ with C++20
python tools/test_tools.py -v             # extractor and validator
```

## Why a native port instead of an emulator

An emulator such as [Xenia](https://xenia.jp) recreates the Xbox 360: it JIT-translates the PowerPC code at run time, emulates the Xenos GPU by processing the console's command buffers and models the audio hardware. That is the right approach for the whole library. This port takes a different route for one game:

- **Ahead-of-time code.** The game's PowerPC modules become C++ compiled by a modern optimizing compiler before you play.
- **No GPU emulation in the hot path.** Draws, clears, resolves and presents of the game's D3D library are hooked and executed directly on Direct3D 11.
- **Native audio** decoding and mixing on the PC.
- **Readable and fixable.** Every hook, renderer path and audio routine is plain C++ in `project/src`.

The trade-off: it covers only this game and revision, it is unfinished, and an emulator is the more general and better-tested option.

## What is in this repository

- `project/` — the CMake project and the port's own C++ code in `project/src/`: native renderer and Xenos shader translation, XMA audio, PC settings menu and game hooks. The ReXGlue manifest says which game functions are translated; the translated code itself is generated on your PC.
- `tools/` — `build_local.py` (downloads the pinned ReXGlue SDK, FFmpeg headers and DXVK, translates your executable, compiles), `play.py`, `extract_iso.py` and `validate_game.py`, `setup.ps1` / `setup_tools.py` (behind `Setup.cmd`), `audit_publication.py` (the check that keeps game data out of this repository) and the shared game-file installer in `tools/port-installer`.
- `patches/` — the small FPSCR patch applied to the SDK headers at build time.
- `kit.json` (pinned SDK, FFmpeg and DXVK downloads), `installer.json` and `setup.json` (what `Install-Files.cmd` and `Setup.cmd` accept and do), `licenses/` and [NOTICE.md](NOTICE.md).
- `tests/` — game-free C++20 tests of the shader and vertex-layout importers (`tools/run_native_tests.py`).

## Credits and licenses

- Port integration, native renderer and audio: [batatinhafcbatatinha-pixel](https://github.com/batatinhafcbatatinha-pixel).
- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk): Tom Clay and contributors, BSD-3-Clause; derived from [Xenia](https://github.com/xenia-project/xenia) (Ben Vanik and contributors). External dependency, downloaded by the build script; license in `licenses/`. The texture untiling and format rules follow Xenia's BSD-licensed `texture_util`; shader-microcode semantics follow the SDK's Xenia-derived headers.
- [FFmpeg](https://ffmpeg.org) (Xenia fork by wmarti), LGPL-2.1: the SDK's prebuilt `libavcodec`/`libavutil` and their public headers, downloaded at build time.
- Microsoft Visual C++ runtime, Windows SDK, Direct3D and XAudio2: Microsoft; obtained separately.
- 2014 FIFA World Cup Brazil, its game code, trademarks and assets belong to Electronic Arts, FIFA and the respective rights holders. This project is not affiliated with or endorsed by EA, FIFA or Microsoft.

See [NOTICE.md](NOTICE.md) for the publication boundary and [LICENSE](LICENSE) for the license of the port's own code.

---

<a id="notice"></a>

**THIS PROJECT DOES NOT INCLUDE THE GAME, AN ISO/XEX, EA GAME ASSETS, GENERATED GAME CODE OR A PRECOMPILED GAME EXECUTABLE. YOU MUST PROVIDE YOUR OWN LAWFULLY OBTAINED XBOX 360 GAME FILES AND HAVE THE RIGHTS REQUIRED FOR YOUR USE. THIS PROJECT DOES NOT AUTHORIZE COPYRIGHT INFRINGEMENT. ALL GAME RIGHTS BELONG TO THEIR RESPECTIVE OWNERS.**
