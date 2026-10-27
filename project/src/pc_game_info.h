// 2014 FIFA World Cup Brazil for the in-game PC settings (pc_settings.h).
#pragma once

#include "pc_settings.h"
#include "native/post_effects.h"

const pcs::Theme* PcCopaTheme();  // pc_theme.cpp

inline pcs::GameInfo PcGameInfo() {
  // data/loc/locale.big ships eng_us and por_br (XC_LANGUAGE ids).
  static const pcs::Language kLanguages[] = {{1, L"English"}, {9, L"Português (Brasil)"}};
  pcs::GameInfo info;
  info.title = L"COPA DO MUNDO DA FIFA BRASIL 2014";
  info.window_title = "2014 FIFA World Cup Brazil";
  info.ini_name = L"copa2014-pc.ini";
  info.languages = kLanguages;
  info.language_count = int(sizeof(kLanguages) / sizeof(kLanguages[0]));
  info.internal_resolution = true;
  static const pcs::Effect kEffects[] = {
      {"brightness_contrast", L"Brilho e contraste cinematográficos", L"Cinematic brightness and contrast",
       L"Ajuste de brilho e contraste aplicado pelo jogo.", L"The game's final brightness and contrast adjustment."},
  };
  static_assert(sizeof(kEffects) / sizeof(kEffects[0]) == ea_postfx::Count);
  info.effects = kEffects;
  info.effect_count = ea_postfx::Count;
  info.football_keyboard = true;
  // Measured 2026-10-20 at a 120 Hz vblank with no limit: the match/drill clock keeps real time at
  // 70-150 fps and the menus do not wait for the vblank, so frame rates above 60 are safe.
  info.real_time = true;
  info.theme = PcCopaTheme();
  return info;
}
