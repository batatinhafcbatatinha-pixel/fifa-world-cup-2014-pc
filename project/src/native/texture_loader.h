// Guest (Xenos) texture memory -> host Direct3D 11 subresources.
// Layout rules (tiling, mip tails, pitches) follow Xenia's texture_util (BSD).

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include <dxgiformat.h>

namespace copa {

struct TextureDesc {
  uint32_t format = 0;       // xenos::TextureFormat
  uint32_t endian = 0;       // xenos::Endian
  uint32_t dimension = 1;    // xenos::DataDimension
  bool tiled = false;
  bool packed_mips = false;
  bool is_signed = false;    // all components signed
  bool gamma = false;        // sign == kGamma
  uint32_t pitch_div32 = 0;  // base row pitch in texels / 32
  uint32_t width = 1, height = 1, depth = 1;  // depth: array size or 3D depth
  uint32_t base_address = 0, mip_address = 0;  // guest physical
  uint32_t mip_min = 0, mip_max = 0;

  bool operator==(const TextureDesc&) const = default;
};

// Decodes a 6-dword texture fetch constant (host-endian dwords).
TextureDesc TextureDescFromFetch(const uint32_t fetch[6]);

struct HostFormatInfo {
  DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
  uint32_t block_width = 1, block_height = 1;
  uint32_t guest_bytes_per_block = 4;
  uint32_t host_bytes_per_block = 4;
  // Converts one guest block row segment (already endian-swapped) to host bytes.
  // nullptr = direct copy.
  void (*convert)(const uint8_t* guest, uint8_t* host, uint32_t blocks) = nullptr;
};

bool GetHostFormat(const TextureDesc& desc, HostFormatInfo& out);

struct LoadedSubresource {
  std::vector<uint8_t> data;
  uint32_t row_pitch = 0, slice_pitch = 0;
};

struct LoadedTexture {
  HostFormatInfo format;
  uint32_t width = 0, height = 0, array_size = 1, mip_levels = 1;
  // Index: level + slice * mip_levels (D3D11 subresource order).
  std::vector<LoadedSubresource> subresources;
};

// `physical` translates a guest physical address to host memory (nullptr if invalid).
bool LoadTexture(const TextureDesc& desc, const std::function<const uint8_t*(uint32_t)>& physical,
                 LoadedTexture& out);

// Bytes of guest memory a texture's base level spans (for hashing/invalidation).
uint32_t GuestBaseExtentBytes(const TextureDesc& desc);

// Tiled 2D address (bytes) for block (x, y) with the pitch in blocks.
int32_t TiledOffset2D(int32_t x, int32_t y, uint32_t pitch_blocks, uint32_t bytes_per_block_log2);

}  // namespace copa
