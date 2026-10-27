// See pc_font.h. Formats, as found on the FIFA Street (2012) and 2014 FIFA World Cup discs:
//
//   BIG4: "BIG4", size (LE), entry count (BE at +8), header size; then per entry offset and size
//   (BE, absolute) and a NUL-terminated name.
//   chunklzx: "chunklzx", then BE version (2), uncompressed size, chunk size (0x40000), chunk
//   count, alignment (16) and a reserved word (0x24 bytes). Each chunk: BE compressed size and
//   type (3 = LZX) right before its data, which starts on the alignment. The data is the Xbox
//   XMemCompress framing: per 32 KB output frame a BE16 size (or 0xFF, BE16 output size, BE16
//   size) and the LZX bytes, ended by a zero size. Every chunk is an independent LZX stream.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "pc_font.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/lzx.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace pcfont {
namespace {

using Bytes = std::vector<uint8_t>;

void Require(bool ok, const char* what) {
  if (!ok) throw std::runtime_error(what);
}

uint32_t BE(const Bytes& b, size_t p, unsigned n = 4) {
  Require(p <= b.size() && n <= b.size() - p, "truncated data");
  uint32_t v = 0;
  for (unsigned i = 0; i < n; ++i) v = (v << 8) | b[p + i];
  return v;
}

// One entry of root/data*.big (the directory is read; only the entry's bytes are loaded).
bool ReadBigEntry(const std::filesystem::path& root, const std::string& entry, Bytes& out) {
  for (int i = 0; i < 16; ++i) {
    std::ifstream f(root / ("data" + std::to_string(i) + ".big"), std::ios::binary);
    if (!f) continue;
    Bytes head(16);
    if (!f.read(reinterpret_cast<char*>(head.data()), 16) || std::memcmp(head.data(), "BIG4", 4)) continue;
    uint32_t count = BE(head, 8), header = BE(head, 12);
    if (count > 200000 || header > 64 * 1024 * 1024) continue;
    Bytes dir(header);
    f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(dir.data()), std::streamsize(dir.size()))) continue;
    size_t p = 16;
    for (uint32_t e = 0; e < count && p + 8 < dir.size(); ++e) {
      uint32_t offset = BE(dir, p), size = BE(dir, p + 4);
      p += 8;
      size_t name = p;
      while (p < dir.size() && dir[p]) ++p;
      if (p >= dir.size()) break;
      ++p;
      if (entry.size() != p - name - 1 || std::memcmp(dir.data() + name, entry.data(), entry.size())) continue;
      Require(size <= 64 * 1024 * 1024, "entry too large");
      out.resize(size);
      f.seekg(offset);
      Require(bool(f.read(reinterpret_cast<char*>(out.data()), std::streamsize(size))), "entry beyond the archive");
      return true;
    }
  }
  return false;
}

Bytes ChunkLzx(const Bytes& in) {
  if (in.size() < 8 || std::memcmp(in.data(), "chunklzx", 8)) return in;  // stored
  uint32_t total = BE(in, 12), chunk = BE(in, 16), count = BE(in, 20), align = BE(in, 24);
  Require(total <= 64 * 1024 * 1024 && chunk && chunk <= 1024 * 1024 && count <= 4096, "bad chunklzx header");
  Require(align && (align & (align - 1)) == 0, "bad chunklzx alignment");
  Bytes out(total);
  size_t cursor = 0x24, written = 0;
  for (uint32_t c = 0; c < count; ++c) {
    size_t data = (cursor + 8 + align - 1) & ~size_t(align - 1);
    uint32_t size = BE(in, data - 8), type = BE(in, data - 4);
    Require(data + size <= in.size(), "chunk beyond the entry");
    size_t want = std::min<size_t>(chunk, total - written);
    if (type == 3) {
      Bytes stream;  // the frames' LZX bytes, without their size prefixes
      size_t p = data, end = data + size;
      while (p + 2 <= end) {
        uint32_t n;
        if (in[p] == 0xFF) {
          n = BE(in, p + 3, 2);
          p += 5;
        } else {
          n = BE(in, p, 2);
          p += 2;
        }
        if (!n || p + n > end) break;
        stream.insert(stream.end(), in.begin() + p, in.begin() + p + n);
        p += n;
      }
      Require(lzx_decompress(stream.data(), stream.size(), out.data() + written, want, 0x20000, nullptr, 0) == 0,
              "LZX error");
    } else {
      Require(size >= want, "short stored chunk");
      std::memcpy(out.data() + written, in.data() + data, want);
    }
    written += want;
    cursor = data + size;
  }
  Require(written == total, "chunklzx size mismatch");
  return out;
}

// The face's weight (OS/2 usWeightClass), so GDI picks this face among a family's styles.
int Weight(const Bytes& ttf) {
  uint32_t tables = BE(ttf, 4, 2);
  for (uint32_t t = 0; t < tables; ++t) {
    size_t rec = 12 + size_t(t) * 16;
    if (BE(ttf, rec) == 0x4F532F32u) return int(BE(ttf, BE(ttf, rec + 8) + 4, 2));  // 'OS/2'
  }
  return FW_NORMAL;
}

// The family name GDI knows the font by: the 'name' table, Windows platform, name ID 1.
std::wstring FamilyName(const Bytes& ttf) {
  uint32_t tables = BE(ttf, 4, 2);
  for (uint32_t t = 0; t < tables; ++t) {
    size_t rec = 12 + size_t(t) * 16;
    if (BE(ttf, rec) != 0x6E616D65u) continue;  // 'name'
    size_t base = BE(ttf, rec + 8);
    uint32_t n = BE(ttf, base + 2, 2);
    size_t strings = base + BE(ttf, base + 4, 2);
    for (uint32_t i = 0; i < n; ++i) {
      size_t r = base + 6 + size_t(i) * 12;
      if (BE(ttf, r, 2) != 3 || BE(ttf, r + 6, 2) != 1) continue;
      uint32_t len = BE(ttf, r + 8, 2), off = BE(ttf, r + 10, 2);
      std::wstring s;
      for (uint32_t k = 0; k + 1 < len; k += 2) s += wchar_t(BE(ttf, strings + off + k, 2));
      if (!s.empty()) return s;
    }
  }
  throw std::runtime_error("font without a family name");
}

}  // namespace

void Rasterize(const Bytes& ttf, const std::string& entry, int pixels, pcs::ThemeFont& out);

std::filesystem::path GameRoot(const char* fallback) {
  auto cvar = rex::cvar::GetFlagByName("game_data_root");
  if (!cvar.empty()) return std::filesystem::u8path(cvar);
  return std::filesystem::u8path(fallback);
}

void LoadTtf(const std::filesystem::path& root, const std::string& entry, int pixels, pcs::ThemeFont& out) {
  Bytes packed;
  Require(ReadBigEntry(root, entry, packed), "font not found in data*.big");
  Rasterize(ChunkLzx(packed), entry, pixels, out);
}

void LoadTtfFile(const std::filesystem::path& file, int pixels, pcs::ThemeFont& out) {
  std::ifstream f(file, std::ios::binary);
  Require(bool(f), "font file missing");
  Bytes ttf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  Rasterize(ttf, file.filename().string(), pixels, out);
}

void Rasterize(const Bytes& ttf, const std::string& entry, int pixels, pcs::ThemeFont& out) {
  Require(ttf.size() > 12, "empty font");
  std::wstring family = FamilyName(ttf);
  DWORD installed = 0;
  // Private to this process: the font is not added to Windows.
  Require(AddFontMemResourceEx(const_cast<uint8_t*>(ttf.data()), DWORD(ttf.size()), nullptr, &installed) && installed,
          "GDI refused the font");
  HFONT font = CreateFontW(-pixels, 0, 0, 0, Weight(ttf), FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, family.c_str());
  Require(font != nullptr, "CreateFont failed");
  HDC dc = CreateCompatibleDC(nullptr);
  SelectObject(dc, font);
  TEXTMETRICW tm{};
  GetTextMetricsW(dc, &tm);
  const int cell_w = tm.tmMaxCharWidth + 4, cell_h = tm.tmHeight + 2;
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = cell_w;
  bi.bmiHeader.biHeight = -cell_h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  uint32_t* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), nullptr, 0);
  SelectObject(dc, bmp);
  SetBkColor(dc, RGB(0, 0, 0));
  SetTextColor(dc, RGB(255, 255, 255));

  std::vector<wchar_t> codes;
  for (wchar_t c = 0x20; c < 0x7F; ++c) codes.push_back(c);
  for (wchar_t c = 0xA0; c <= 0xFF; ++c) codes.push_back(c);
  for (wchar_t c = 0x400; c < 0x460; ++c) codes.push_back(c);
  for (wchar_t c : {0x152, 0x153, 0x2022, 0x2014, 0x2019, 0x201C, 0x201D, 0x25C0, 0x25B6}) codes.push_back(wchar_t(c));
  // Only glyphs the font really has (GDI would substitute another font's).
  std::vector<WORD> index(codes.size());
  GetGlyphIndicesW(dc, codes.data(), int(codes.size()), index.data(), GGI_MARK_NONEXISTING_GLYPHS);

  const int atlas_w = 1024;
  int x = 0, y = 0;
  out = pcs::ThemeFont{};
  struct Pending { pcs::ThemeGlyph g; std::vector<uint8_t> alpha; };
  std::vector<Pending> pending;
  for (size_t i = 0; i < codes.size(); ++i) {
    if (index[i] == 0xFFFF && codes[i] != L' ') continue;
    ABC abc{};
    if (!GetCharABCWidthsW(dc, codes[i], codes[i], &abc)) continue;
    std::memset(bits, 0, size_t(cell_w) * cell_h * 4);
    int left = abc.abcA < 0 ? -abc.abcA : 0;
    TextOutW(dc, 2 + left, 0, &codes[i], 1);
    GdiFlush();
    int x0 = cell_w, x1 = -1, y0 = cell_h, y1 = -1;
    for (int yy = 0; yy < cell_h; ++yy)
      for (int xx = 0; xx < cell_w; ++xx)
        if (bits[yy * cell_w + xx] & 0xFF) {
          x0 = std::min(x0, xx); x1 = std::max(x1, xx);
          y0 = std::min(y0, yy); y1 = std::max(y1, yy);
        }
    Pending p{};
    p.g.code = uint16_t(codes[i]);
    p.g.advance = uint16_t(std::max(0, int(abc.abcA + abc.abcB + abc.abcC)));
    if (x1 >= x0) {
      int w = x1 - x0 + 1, h = y1 - y0 + 1;
      p.g.width = uint8_t(std::min(w, 255));
      p.g.height = uint8_t(std::min(h, 255));
      p.g.offset_x = int8_t(std::clamp(x0 - 2 - left, -128, 127));
      p.g.offset_y = uint8_t(std::clamp(y0, 0, 255));
      for (int yy = 0; yy < p.g.height; ++yy)
        for (int xx = 0; xx < p.g.width; ++xx)
          p.alpha.push_back(uint8_t(bits[(y0 + yy) * cell_w + x0 + xx] & 0xFF));
    }
    pending.push_back(std::move(p));
  }
  // Shelf packing in a 1024-wide atlas.
  int shelf = 0;
  for (Pending& p : pending) {
    if (x + p.g.width + 1 > atlas_w) { x = 0; y += shelf + 1; shelf = 0; }
    p.g.x = uint16_t(x);
    p.g.y = uint16_t(y);
    x += p.g.width + 1;
    shelf = std::max(shelf, int(p.g.height));
  }
  out.atlas.width = atlas_w;
  out.atlas.height = y + shelf + 1;
  out.atlas.rgba.assign(size_t(out.atlas.width) * out.atlas.height, 0);
  for (const Pending& p : pending) {
    for (int yy = 0; yy < p.g.height; ++yy)
      for (int xx = 0; xx < p.g.width; ++xx)
        out.atlas.rgba[size_t(p.g.y + yy) * atlas_w + p.g.x + xx] = uint32_t(p.alpha[yy * p.g.width + xx]) << 24;
    out.glyphs.push_back(p.g);
  }
  out.line_height = tm.tmHeight;
  DeleteObject(bmp);
  DeleteDC(dc);
  DeleteObject(font);
  REXLOG_INFO("[pc] font {} ({}): {} glyphs at {} px", entry, std::string(family.begin(), family.end()),
              out.glyphs.size(), pixels);
}

}  // namespace pcfont

namespace pcfont {
std::filesystem::path ExeDir() {
  wchar_t path[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  return std::filesystem::path(std::wstring(path, n)).parent_path();
}
}  // namespace pcfont
