// copa2014 - ReXGlue Recompiled Project (Copa do Mundo da FIFA Brasil 2014)

#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>

#include <rex/input/input_system.h>

#include "native/copa_audio.h"
#include "native/copa_graphics.h"
#include "pc_game_info.h"
#include "script_input.h"

class Copa2014App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Copa2014App>(new Copa2014App(ctx, "copa2014",
        PPCImageConfig));
  }

 protected:
  // Game files: --game_data_root wins; otherwise <exe>/game_files, <exe>/game, then the
  // extracted disc on D:.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    // Portable defaults keep saves and caches beside this port.
    // Explicit command-line/environment paths still take precedence.
    auto portable_dir = rex::filesystem::GetExecutableFolder() / "userdata";
    if (rex::cvar::GetFlagByName("user_data_root").empty())
      paths.user_data_root = portable_dir;
    if (rex::cvar::GetFlagByName("cache_root").empty())
      paths.cache_root = paths.user_data_root / "cache";
    auto has_xex = [](const std::filesystem::path& p) {
      std::error_code ec;
      return !p.empty() && std::filesystem::is_regular_file(p / "default.xex", ec);
    };
    if (has_xex(paths.game_data_root)) return;
    auto exe_dir = rex::filesystem::GetExecutableFolder();
    for (auto candidate : {exe_dir / "game_files", exe_dir / "game",
                           std::filesystem::path("D:/Copa2014-game")}) {
      if (has_xex(candidate)) {
        paths.game_data_root = std::filesystem::weakly_canonical(candidate);
        return;
      }
    }
  }

  // Native graphics only: no GPU plugin, no Xenos command processor.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    // In-game PC settings (F1): window size/mode and game language from copa2014-pc.ini.
    pcs::Configure(PcGameInfo());
    config.gpu_plugin.clear();
    config.graphics = std::make_unique<copa::NativeGraphicsSystem>();
    // COPA_INPUT=<file>: scripted pad (see script_input.h) next to the real ones.
    if (const char* script = std::getenv("COPA_INPUT"); script && *script) {
      config.input_factory = [path = std::string(script)](bool tool_mode)
          -> std::unique_ptr<rex::system::IInputSystem> {
        auto input = rex::input::CreateDefaultInputSystem(tool_mode);
        auto driver = std::make_unique<ScriptInputDriver>(path);
        driver->Setup();
        input->AddDriver(std::move(driver));
        return input;
      };
    }
    // Native audio only: no APU / XMA register emulation.
    config.audio_factory = [](rex::runtime::FunctionDispatcher*)
        -> std::unique_ptr<rex::system::IAudioSystem> {
      return std::make_unique<copa::NativeAudioSystem>();
    };
  }
};
