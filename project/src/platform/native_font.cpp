#include "native_font.h"
#include "../pc_settings.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <algorithm>
#include <stdexcept>

namespace native_host {
void RasterizeFont(const std::vector<uint8_t>& data, int pixels, pcs::ThemeFont& output) {
  if (data.empty() || data.size() > 64 * 1024 * 1024 || pixels <= 0 || pixels > 255)
    throw std::runtime_error("Invalid font data or pixel size");
  struct Library {
    FT_Library value = nullptr;
    ~Library() { if (value) FT_Done_FreeType(value); }
  } library;
  if (FT_Init_FreeType(&library.value)) throw std::runtime_error("FreeType initialization failed");
  struct Face {
    FT_Face value = nullptr;
    ~Face() { if (value) FT_Done_Face(value); }
  } face;
  if (FT_New_Memory_Face(library.value, data.data(), static_cast<FT_Long>(data.size()), 0, &face.value) ||
      FT_Select_Charmap(face.value, FT_ENCODING_UNICODE) ||
      FT_Set_Pixel_Sizes(face.value, 0, pixels))
    throw std::runtime_error("FreeType refused the font");
  const int ascender = int((face.value->size->metrics.ascender + 63) / 64);
  std::vector<uint16_t> codes;
  for (uint16_t c = 0x20; c < 0x7f; ++c) codes.push_back(c);
  for (uint16_t c = 0xa0; c <= 0xff; ++c) codes.push_back(c);
  for (uint16_t c = 0x400; c < 0x460; ++c) codes.push_back(c);
  for (auto c : {0x152, 0x153, 0x2022, 0x2014, 0x2019, 0x201c, 0x201d, 0x25c0, 0x25b6})
    codes.push_back(uint16_t(c));
  struct Pending { pcs::ThemeGlyph glyph{}; std::vector<uint8_t> alpha; };
  std::vector<Pending> pending;
  int x = 0, y = 0, shelf = 0;
  for (uint16_t code : codes) {
    const auto index = FT_Get_Char_Index(face.value, code);
    if (!index && code != 0x20) continue;
    if (FT_Load_Glyph(face.value, index, FT_LOAD_DEFAULT) ||
        FT_Render_Glyph(face.value->glyph, FT_RENDER_MODE_NORMAL)) continue;
    const auto slot = face.value->glyph;
    const auto& bitmap = slot->bitmap;
    if (bitmap.width && bitmap.rows && bitmap.pixel_mode != FT_PIXEL_MODE_GRAY)
      throw std::runtime_error("Unsupported FreeType bitmap format");
    Pending p;
    auto& g = p.glyph;
    g.code = code;
    g.width = uint8_t(std::min(bitmap.width, 255u));
    g.height = uint8_t(std::min(bitmap.rows, 255u));
    g.advance = uint16_t(std::clamp<FT_Pos>((slot->advance.x + 32) / 64, 0, 65535));
    g.offset_x = int8_t(std::clamp(slot->bitmap_left, -128, 127));
    g.offset_y = uint8_t(std::clamp(ascender - slot->bitmap_top, 0, 255));
    p.alpha.resize(size_t(g.width) * g.height);
    for (unsigned row = 0; row < g.height; ++row) {
      const auto* src = bitmap.pitch >= 0 ? bitmap.buffer + row * bitmap.pitch
          : bitmap.buffer + (bitmap.rows - 1 - row) * -bitmap.pitch;
      for (unsigned col = 0; col < g.width; ++col)
        p.alpha[size_t(row) * g.width + col] = bitmap.num_grays > 1
            ? uint8_t(unsigned(src[col]) * 255 / (bitmap.num_grays - 1)) : 0;
    }
    if (x + g.width + 1 > 1024) { x = 0; y += shelf + 1; shelf = 0; }
    g.x = uint16_t(x); g.y = uint16_t(y);
    x += g.width + 1;
    shelf = std::max(shelf, int(g.height));
    pending.push_back(std::move(p));
  }
  pcs::ThemeFont result;
  result.line_height = std::max(1, int((face.value->size->metrics.height + 63) / 64));
  result.atlas.width = 1024;
  result.atlas.height = y + shelf + 1;
  result.atlas.rgba.assign(size_t(result.atlas.width) * result.atlas.height, 0);
  for (const auto& p : pending) {
    const auto& g = p.glyph;
    for (unsigned row = 0; row < g.height; ++row)
      for (unsigned col = 0; col < g.width; ++col)
        result.atlas.rgba[size_t(g.y + row) * 1024 + g.x + col] =
            uint32_t(p.alpha[size_t(row) * g.width + col]) << 24;
    result.glyphs.push_back(g);
  }
  output = std::move(result);
}
}
