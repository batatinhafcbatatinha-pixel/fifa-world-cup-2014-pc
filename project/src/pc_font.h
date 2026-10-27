// The game's own TrueType fonts for the PC settings menu, read at run time from the user's disc
// archives (never shipped here): an entry of data*.big (BIG4), compressed as EA "chunklzx",
// becomes a TTF that GDI rasterizes into a pcs::ThemeFont atlas.
#pragma once

#include <filesystem>
#include <string>

#include "pc_settings.h"

namespace pcfont {

// The game files: --game_data_root, else `fallback`.
std::filesystem::path GameRoot(const char* fallback);

// Rasterizes `entry` (e.g. "data/ui/fonts/fs4_header.ttf", searched in root/data*.big) at
// `pixels` em height into `out` (Latin, Latin-1, Cyrillic and a few symbols). Throws on failure.
void LoadTtf(const std::filesystem::path& root, const std::string& entry, int pixels, pcs::ThemeFont& out);

// The same from a TTF file (a font the port may ship, such as a freely licensed one).
void LoadTtfFile(const std::filesystem::path& file, int pixels, pcs::ThemeFont& out);

// The executable's folder (fonts shipped with the port live in its fonts\ subfolder).
std::filesystem::path ExeDir();

}  // namespace pcfont
