// 2014 FIFA World Cup Brazil for the in-game PC settings (pc_settings.h).
#pragma once

#include "pc_settings.h"

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
  info.football_keyboard = true;
  return info;
}
