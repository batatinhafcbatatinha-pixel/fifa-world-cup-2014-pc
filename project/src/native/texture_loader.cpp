// Guest texture loading. Tiling and mip layout follow Xenia's texture_util.cc (BSD,
// Copyright Ben Vanik), rewritten for this renderer.

#include "texture_loader.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace copa {

namespace {

uint32_t Log2Ceil(uint32_t v) { return v <= 1 ? 0 : 32 - std::countl_zero(v - 1); }
uint32_t Log2Floor(uint32_t v) { return v ? 31 - std::countl_zero(v) : 0; }
uint32_t NextPow2(uint32_t v) { return v <= 1 ? 1 : 1u << Log2Ceil(v); }
uint32_t Align(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

void SwapCopy(uint32_t endian, uint8_t* dst, const uint8_t* src, uint32_t bytes) {
  switch (endian) {
    case 1:  // 8in16
      for (uint32_t i = 0; i + 1 < bytes; i += 2) {
        dst[i] = src[i + 1];
        dst[i + 1] = src[i];
      }
      break;
    case 2:  // 8in32
      for (uint32_t i = 0; i + 3 < bytes; i += 4) {
        dst[i] = src[i + 3];
        dst[i + 1] = src[i + 2];
        dst[i + 2] = src[i + 1];
        dst[i + 3] = src[i];
      }
      break;
    case 3:  // 16in32
      for (uint32_t i = 0; i + 3 < bytes; i += 4) {
        dst[i] = src[i + 2];
        dst[i + 1] = src[i + 3];
        dst[i + 2] = src[i];
        dst[i + 3] = src[i + 1];
      }
      break;
    default:
      std::memcpy(dst, src, bytes);
  }
}

uint16_t FloatToHalf(float f) {
  uint32_t x;
  std::memcpy(&x, &f, 4);
  uint32_t sign = (x >> 16) & 0x8000;
  int32_t exp = int32_t((x >> 23) & 0xFF) - 127 + 15;
  uint32_t mant = x & 0x7FFFFF;
  if (exp <= 0) return uint16_t(sign);
  if (exp >= 31) return uint16_t(sign | 0x7C00);
  return uint16_t(sign | (exp << 10) | (mant >> 13));
}

float Float7e3To32(uint32_t f10) {
  uint32_t mantissa = f10 & 0x7F, exponent = (f10 >> 7) & 0x7;
  if (!exponent) return float(mantissa) / 128.0f * 0.25f;  // denormal, 2^(1-3)
  return (1.0f + float(mantissa) / 128.0f) * std::ldexp(1.0f, int(exponent) - 3);
}

float Float20e4To32(uint32_t f24) {
  uint32_t mantissa = f24 & 0xFFFFF, exponent = f24 >> 20;
  if (!exponent) return float(mantissa) / float(1 << 20) * std::ldexp(1.0f, -14);
  return (1.0f + float(mantissa) / float(1 << 20)) * std::ldexp(1.0f, int(exponent) - 15);
}

void Conv6_5_5(const uint8_t* g, uint8_t* h, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) {
    uint16_t v = uint16_t(g[i * 2] | (g[i * 2 + 1] << 8));
    h[i * 4 + 0] = uint8_t(((v >> 0) & 31) * 255 / 31);
    h[i * 4 + 1] = uint8_t(((v >> 5) & 31) * 255 / 31);
    h[i * 4 + 2] = uint8_t(((v >> 10) & 63) * 255 / 63);
    h[i * 4 + 3] = 255;
  }
}
void Conv4_4_4_4(const uint8_t* g, uint8_t* h, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) {
    uint16_t v = uint16_t(g[i * 2] | (g[i * 2 + 1] << 8));
    for (int c = 0; c < 4; ++c) h[i * 4 + c] = uint8_t(((v >> (4 * c)) & 15) * 17);
  }
}
void Conv10_11_11(const uint8_t* g, uint8_t* h, uint32_t n) {  // x11 y11 z10 unorm
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t v;
    std::memcpy(&v, g + i * 4, 4);
    uint16_t out[4] = {uint16_t((v & 0x7FF) * 65535 / 0x7FF),
                       uint16_t(((v >> 11) & 0x7FF) * 65535 / 0x7FF),
                       uint16_t(((v >> 22) & 0x3FF) * 65535 / 0x3FF), 65535};
    std::memcpy(h + i * 8, out, 8);
  }
}
void Conv11_11_10(const uint8_t* g, uint8_t* h, uint32_t n) {  // x10 y11 z11 unorm
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t v;
    std::memcpy(&v, g + i * 4, 4);
    uint16_t out[4] = {uint16_t((v & 0x3FF) * 65535 / 0x3FF),
                       uint16_t(((v >> 10) & 0x7FF) * 65535 / 0x7FF),
                       uint16_t(((v >> 21) & 0x7FF) * 65535 / 0x7FF), 65535};
    std::memcpy(h + i * 8, out, 8);
  }
}
void Conv24_8(const uint8_t* g, uint8_t* h, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t v;
    std::memcpy(&v, g + i * 4, 4);
    float f = float(v >> 8) / float(0xFFFFFF);
    std::memcpy(h + i * 4, &f, 4);
  }
}
void Conv24_8F(const uint8_t* g, uint8_t* h, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t v;
    std::memcpy(&v, g + i * 4, 4);
    float f = Float20e4To32(v >> 8);
    std::memcpy(h + i * 4, &f, 4);
  }
}
void Conv2_10_10_10F(const uint8_t* g, uint8_t* h, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t v;
    std::memcpy(&v, g + i * 4, 4);
    uint16_t out[4] = {FloatToHalf(Float7e3To32(v & 0x3FF)),
                       FloatToHalf(Float7e3To32((v >> 10) & 0x3FF)),
                       FloatToHalf(Float7e3To32((v >> 20) & 0x3FF)),
                       FloatToHalf(float(v >> 30) / 3.0f)};
    std::memcpy(h + i * 8, out, 8);
  }
}
void ConvDxt3A(const uint8_t* g, uint8_t* h, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) {
    std::memcpy(h + i * 16, g + i * 8, 8);
    std::memset(h + i * 16 + 8, 0, 8);
  }
}

constexpr uint32_t kCtx1 = 60;

}  // namespace

int32_t TiledOffset2D(int32_t x, int32_t y, uint32_t pitch, uint32_t log2_bpb) {
  pitch = Align(pitch, 32);
  int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (log2_bpb + 7);
  int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bpb;
  int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// 3D tiling (32x16x4 macro tiles; Xenia texture_address.h, BSD).
int64_t TiledOffset3D(int32_t x, int32_t y, int32_t z, uint32_t pitch, uint32_t height,
                      uint32_t log2_bpb) {
  pitch = Align(pitch, 32);
  height = Align(height, 32);
  int32_t outer = ((((z >> 2) * int32_t(height >> 4) + (y >> 4)) * int32_t(pitch >> 5)) + (x >> 5)) << 7;
  int32_t inner = ((z & 3) << 5) | (((y >> 1) & 3) << 3) | (x & 7);
  int64_t bytes = int64_t(outer | inner) << log2_bpb;
  uint32_t bank = ((y >> 3) ^ (z >> 2)) & 1;
  uint32_t pipe = ((x >> 3) & 3) ^ (bank << 1);
  return int64_t((uint32_t(y & 1) << 4) | (pipe << 6) | (bank << 11)) | (bytes & 0xF) |
         (((bytes >> 4) & 1) << 5) | (((bytes >> 5) & 7) << 8) | (bytes >> 8 << 12);
}

TextureDesc TextureDescFromFetch(const uint32_t f[6]) {
  TextureDesc d;
  uint32_t sign_x = (f[0] >> 2) & 3, sign_y = (f[0] >> 4) & 3, sign_z = (f[0] >> 6) & 3,
           sign_w = (f[0] >> 8) & 3;
  d.is_signed = sign_x == 1 && sign_y == 1 && sign_z == 1 && sign_w == 1;
  d.gamma = sign_x == 3;
  d.pitch_div32 = (f[0] >> 22) & 0x1FF;
  d.tiled = (f[0] >> 31) & 1;
  d.format = f[1] & 0x3F;
  d.endian = (f[1] >> 6) & 3;
  bool stacked = (f[1] >> 10) & 1;
  uint32_t base_page = (f[1] >> 12) & 0x1FFFF;
  d.dimension = (f[5] >> 9) & 3;
  d.packed_mips = (f[5] >> 11) & 1;
  uint32_t mip_page = (f[5] >> 12) & 0x1FFFF;
  switch (d.dimension) {
    case 0:  // 1D
      d.width = (f[2] & 0xFFFFFF) + 1;
      break;
    case 1:  // 2D or stacked
      d.width = (f[2] & 0x1FFF) + 1;
      d.height = ((f[2] >> 13) & 0x1FFF) + 1;
      d.depth = stacked ? ((f[2] >> 26) & 0x3F) + 1 : 1;
      break;
    case 2:  // 3D
      d.width = (f[2] & 0x7FF) + 1;
      d.height = ((f[2] >> 11) & 0x7FF) + 1;
      d.depth = ((f[2] >> 22) & 0x3FF) + 1;
      break;
    case 3:  // cube
      d.width = (f[2] & 0x1FFF) + 1;
      d.height = ((f[2] >> 13) & 0x1FFF) + 1;
      d.depth = 6;
      break;
  }
  uint32_t longest = std::max(d.width, d.height);
  if (d.dimension == 2) longest = std::max(longest, d.depth);
  uint32_t size_max_level = Log2Floor(longest);
  uint32_t mip_min = (f[4] >> 2) & 0xF, mip_max = (f[4] >> 6) & 0xF;
  if (mip_page == 0) {
    mip_min = mip_max = 0;
  } else {
    mip_min = std::min(mip_min, size_max_level);
    mip_max = std::max(std::min(mip_max, size_max_level), mip_min);
  }
  if (mip_max != 0) {
    if (base_page == 0) mip_min = std::max(mip_min, 1u);
    if (mip_min != 0) base_page = 0;
  } else {
    mip_page = 0;
  }
  d.base_address = base_page << 12;
  d.mip_address = mip_page << 12;
  d.mip_min = mip_min;
  d.mip_max = mip_max;
  return d;
}

bool GetHostFormat(const TextureDesc& d, HostFormatInfo& o) {
  o = HostFormatInfo{};
  auto set = [&](DXGI_FORMAT f, uint32_t gbpb, uint32_t hbpb = 0, uint32_t bw = 1,
                 uint32_t bh = 1) {
    o.dxgi = f;
    o.guest_bytes_per_block = gbpb;
    o.host_bytes_per_block = hbpb ? hbpb : gbpb;
    o.block_width = bw;
    o.block_height = bh;
  };
  bool s = d.is_signed, g = d.gamma;
  switch (d.format) {
    case 2: case 8: case 9: set(s ? DXGI_FORMAT_R8_SNORM : DXGI_FORMAT_R8_UNORM, 1); break;
    case 3: set(DXGI_FORMAT_B5G5R5A1_UNORM, 2); break;
    case 4: set(DXGI_FORMAT_B5G6R5_UNORM, 2); break;
    case 5: set(DXGI_FORMAT_R8G8B8A8_UNORM, 2, 4); o.convert = Conv6_5_5; break;
    case 6: case 14: case 50:
      set(s ? DXGI_FORMAT_R8G8B8A8_SNORM
            : g ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM, 4);
      break;
    case 62: set(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 4); break;
    case 7: case 54: set(DXGI_FORMAT_R10G10B10A2_UNORM, 4); break;
    case 10: set(s ? DXGI_FORMAT_R8G8_SNORM : DXGI_FORMAT_R8G8_UNORM, 2); break;
    case 11: set(DXGI_FORMAT_R8G8_B8G8_UNORM, 4, 4, 2, 1); break;
    case 12: set(DXGI_FORMAT_G8R8_G8B8_UNORM, 4, 4, 2, 1); break;
    case 13: set(DXGI_FORMAT_R16G16_SNORM, 4); break;
    case 15: set(DXGI_FORMAT_R8G8B8A8_UNORM, 2, 4); o.convert = Conv4_4_4_4; break;
    case 16: case 55: set(DXGI_FORMAT_R16G16B16A16_UNORM, 4, 8); o.convert = Conv10_11_11; break;
    case 17: case 56: set(DXGI_FORMAT_R16G16B16A16_UNORM, 4, 8); o.convert = Conv11_11_10; break;
    case 18: case 51: set(g ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM, 8, 8, 4, 4); break;
    case 19: case 52: set(g ? DXGI_FORMAT_BC2_UNORM_SRGB : DXGI_FORMAT_BC2_UNORM, 16, 16, 4, 4); break;
    case 20: case 53: set(g ? DXGI_FORMAT_BC3_UNORM_SRGB : DXGI_FORMAT_BC3_UNORM, 16, 16, 4, 4); break;
    case 21: set(DXGI_FORMAT_R16G16B16A16_SNORM, 8); break;
    case 22: set(DXGI_FORMAT_R32_FLOAT, 4); o.convert = Conv24_8; break;
    case 23: set(DXGI_FORMAT_R32_FLOAT, 4); o.convert = Conv24_8F; break;
    case 24: set(s ? DXGI_FORMAT_R16_SNORM : DXGI_FORMAT_R16_UNORM, 2); break;
    case 25: set(s ? DXGI_FORMAT_R16G16_SNORM : DXGI_FORMAT_R16G16_UNORM, 4); break;
    case 26: set(s ? DXGI_FORMAT_R16G16B16A16_SNORM : DXGI_FORMAT_R16G16B16A16_UNORM, 8); break;
    case 27: case 30: set(DXGI_FORMAT_R16_FLOAT, 2); break;
    case 28: case 31: set(DXGI_FORMAT_R16G16_FLOAT, 4); break;
    case 29: case 32: set(DXGI_FORMAT_R16G16B16A16_FLOAT, 8); break;
    case 33: case 36: set(DXGI_FORMAT_R32_FLOAT, 4); break;
    case 34: case 37: set(DXGI_FORMAT_R32G32_FLOAT, 8); break;
    case 35: case 38: set(DXGI_FORMAT_R32G32B32A32_FLOAT, 16); break;
    case 49: set(s ? DXGI_FORMAT_BC5_SNORM : DXGI_FORMAT_BC5_UNORM, 16, 16, 4, 4); break;
    case 57: set(DXGI_FORMAT_R32G32B32_FLOAT, 12); break;
    case 58: case 61: set(DXGI_FORMAT_BC2_UNORM, 8, 16, 4, 4); o.convert = ConvDxt3A; break;
    case 59: set(s ? DXGI_FORMAT_BC4_SNORM : DXGI_FORMAT_BC4_UNORM, 8, 8, 4, 4); break;
    case kCtx1: set(DXGI_FORMAT_R8G8_UNORM, 8, 2, 4, 4); break;  // decoded specially
    case 63: set(DXGI_FORMAT_R16G16B16A16_FLOAT, 4, 8); o.convert = Conv2_10_10_10F; break;
    default: return false;
  }
  return true;
}

namespace {

struct LevelLayout {
  uint32_t address;        // guest physical address of level (or of the packed tail)
  uint32_t row_pitch;      // bytes per block row (linear)
  uint32_t pitch_blocks;   // pitch in blocks (tiled)
  uint32_t slice_stride;   // bytes between array slices
  uint32_t z_stride = 0;   // bytes between depth slices of a 3D level
  uint32_t rows = 0;       // block rows, aligned to 32
  uint32_t offset_x = 0, offset_y = 0;  // packed tail offset in blocks
};

uint32_t PackedMipLevel(uint32_t w, uint32_t h) {
  uint32_t l = Log2Ceil(std::min(w, h));
  return l > 4 ? l - 4 : 0;
}

void PackedMipOffset(uint32_t width, uint32_t height, uint32_t mip, uint32_t bw, uint32_t bh,
                     uint32_t& x, uint32_t& y) {
  uint32_t lw = Log2Ceil(width), lh = Log2Ceil(height);
  uint32_t ls = std::min(lw, lh);
  uint32_t base = ls > 4 ? ls - 4 : 0;
  uint32_t packed = mip - base;
  if (packed < 3) {
    if (lw > lh) { x = 0; y = 16 >> packed; } else { x = 16 >> packed; y = 0; }
  } else if (lw > lh) {
    x = (1u << (lw - base)) >> (packed - 2);
    y = 0;
  } else {
    x = 0;
    y = (1u << (lh - base)) >> (packed - 2);
  }
  x /= bw;
  y /= bh;
}

// Pitch/stride of a stored level (non-packed or packed tail level).
void LevelPitch(const TextureDesc& d, const HostFormatInfo& f, uint32_t level, LevelLayout& l) {
  uint32_t pitch_texels, rows_texels;
  if (level == 0) {
    pitch_texels = d.pitch_div32 << 5;
    rows_texels = d.height;
  } else {
    pitch_texels = std::max(NextPow2(d.width) >> level, 1u);
    rows_texels = std::max(NextPow2(d.height) >> level, 1u);
  }
  uint32_t pitch_blocks = Align(Align(pitch_texels, f.block_width) / f.block_width, 32);
  l.pitch_blocks = pitch_blocks;
  l.row_pitch = pitch_blocks * f.guest_bytes_per_block;
  if (!d.tiled) {
    if (level == 0)
      l.row_pitch = Align(pitch_texels, f.block_width) / f.block_width * f.guest_bytes_per_block;
    else
      l.row_pitch = Align(l.row_pitch, 256);
  }
  uint32_t rows = d.dimension == 0 ? 1 : Align(Align(rows_texels, f.block_height) / f.block_height, 32);
  uint32_t stride = l.row_pitch * rows;
  l.rows = rows;
  l.z_stride = stride;
  if (d.dimension == 2) stride *= Align(d.depth, 4);
  l.slice_stride = Align(stride, 4096);
}

}  // namespace

uint32_t GuestBaseExtentBytes(const TextureDesc& d) {
  HostFormatInfo f;
  if (!GetHostFormat(d, f)) return 0;
  LevelLayout l{};
  LevelPitch(d, f, 0, l);
  uint32_t slices = d.dimension == 2 ? 1 : d.depth;
  return l.slice_stride * slices;
}

bool LoadTexture(const TextureDesc& d, const std::function<const uint8_t*(uint32_t)>& physical,
                 LoadedTexture& out) {
  HostFormatInfo f;
  if (!GetHostFormat(d, f)) return false;
  out.format = f;
  bool has_base = d.base_address != 0;
  uint32_t first_level = has_base ? 0 : 1;
  uint32_t last_level = std::max(d.mip_max, first_level);
  // 3D textures: the base level only (its depth slices become the array / volume slices).
  if (d.dimension == 2) last_level = first_level;
  if (!has_base && !d.mip_address) return false;
  out.width = std::max(d.width >> first_level, 1u);
  out.height = std::max(d.height >> first_level, 1u);
  out.array_size = d.dimension == 2 ? d.depth : d.depth;  // 3D stored as slices
  out.mip_levels = last_level - first_level + 1;
  out.subresources.assign(size_t(out.mip_levels) * out.array_size, {});

  uint32_t packed_level = d.packed_mips ? PackedMipLevel(d.width, d.height) : UINT32_MAX;
  uint32_t log2_bpb = Log2Floor(f.guest_bytes_per_block);

  // Mip chain offsets (levels >= 1 stored from mip_address).
  std::vector<uint32_t> mip_offsets(last_level + 2, 0);
  {
    uint32_t offset = 0;
    for (uint32_t level = 1; level <= last_level; ++level) {
      mip_offsets[level] = offset;
      if (level >= packed_level) break;
      LevelLayout l{};
      LevelPitch(d, f, level, l);
      offset += l.slice_stride * (d.dimension == 2 ? 1 : d.depth);
    }
  }

  // Physical memory is one contiguous host mapping.
  const uint8_t* mem = physical(0);
  if (!mem) return false;
  std::vector<uint8_t> block(64);
  for (uint32_t level = first_level; level <= last_level; ++level) {
    uint32_t stored_level = std::min(level, packed_level);
    LevelLayout l{};
    LevelPitch(d, f, stored_level, l);
    l.address = stored_level == 0 ? d.base_address : d.mip_address + mip_offsets[stored_level];
    if (level >= packed_level)
      PackedMipOffset(d.width, d.height, level, f.block_width, f.block_height, l.offset_x, l.offset_y);
    uint32_t lw = std::max(d.width >> level, 1u), lh = std::max(d.height >> level, 1u);
    uint32_t bx_count = (lw + f.block_width - 1) / f.block_width;
    uint32_t by_count = (lh + f.block_height - 1) / f.block_height;
    for (uint32_t slice = 0; slice < out.array_size; ++slice) {
      auto& sub = out.subresources[size_t(level - first_level) + size_t(slice) * out.mip_levels];
      bool ctx1 = d.format == kCtx1;
      uint32_t host_row = ctx1 ? lw * 2 : bx_count * f.host_bytes_per_block;
      uint32_t host_rows = ctx1 ? lh : by_count;
      sub.row_pitch = host_row;
      sub.slice_pitch = host_row * host_rows;
      sub.data.assign(sub.slice_pitch, 0);
      const bool volume = d.dimension == 2;
      uint32_t slice_base = l.address + slice * (volume ? (d.tiled ? 0 : l.z_stride) : l.slice_stride);
      for (uint32_t by = 0; by < by_count; ++by) {
        for (uint32_t bx = 0; bx < bx_count; ++bx) {
          uint32_t x = bx + l.offset_x, y = by + l.offset_y;
          uint32_t src;
          if (!d.tiled)
            src = slice_base + y * l.row_pitch + x * f.guest_bytes_per_block;
          else if (volume)
            src = slice_base + uint32_t(TiledOffset3D(int32_t(x), int32_t(y), int32_t(slice),
                                                      l.pitch_blocks, l.rows, log2_bpb));
          else
            src = slice_base + uint32_t(TiledOffset2D(int32_t(x), int32_t(y), l.pitch_blocks, log2_bpb));
          if ((src & 0x1FFFFFFF) + f.guest_bytes_per_block > 0x20000000u) continue;
          const uint8_t* p = mem + (src & 0x1FFFFFFF);
          SwapCopy(d.endian, block.data(), p, f.guest_bytes_per_block);
          if (ctx1) {
            uint8_t g0 = block[0], r0 = block[1], g1 = block[2], r1 = block[3];
            uint32_t bits;
            std::memcpy(&bits, block.data() + 4, 4);
            uint8_t cr[4] = {r0, r1, uint8_t((2 * r0 + r1) / 3), uint8_t((r0 + 2 * r1) / 3)};
            uint8_t cg[4] = {g0, g1, uint8_t((2 * g0 + g1) / 3), uint8_t((g0 + 2 * g1) / 3)};
            for (uint32_t oy = 0; oy < 4; ++oy)
              for (uint32_t ox = 0; ox < 4; ++ox) {
                uint32_t px = bx * 4 + ox, py = by * 4 + oy;
                if (px >= lw || py >= lh) continue;
                uint32_t idx = (bits >> ((ox + oy * 4) * 2)) & 3;
                sub.data[py * host_row + px * 2] = cr[idx];
                sub.data[py * host_row + px * 2 + 1] = cg[idx];
              }
            continue;
          }
          uint8_t* dst = sub.data.data() + by * host_row + bx * f.host_bytes_per_block;
          if (f.convert)
            f.convert(block.data(), dst, 1);
          else
            std::memcpy(dst, block.data(), f.host_bytes_per_block);
        }
      }
    }
  }
  return true;
}

}  // namespace copa
