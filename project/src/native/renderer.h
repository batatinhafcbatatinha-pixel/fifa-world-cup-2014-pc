// Copa 2014 native renderer (Direct3D 11). Fed by hooks on the game's D3D library
// (d3d_hooks.cpp); reads pipeline state from the guest device and draws directly with
// D3D11. No GPU command buffer is parsed or executed.

#pragma once

#include <cstdint>

#include "guest_device.h"

namespace copa {

struct DrawRequest {
  uint32_t primitive = 4;  // xenos::PrimitiveType
  bool indexed = false;
  uint32_t index_address = 0;  // guest virtual address of the first index
  bool index32 = false;
  uint32_t count = 0;          // index count (indexed) or vertex count
  uint32_t first_vertex = 0;   // non-indexed: index of the first vertex
  int32_t base_vertex = 0;     // added to every index (VGT_INDX_OFFSET)
  // Inline (DrawVerticesUP-style) data replaces one vertex fetch constant.
  bool vfetch_override = false;
  uint32_t override_index = 95;
  uint32_t override_address = 0;  // guest virtual
  uint32_t override_bytes = 0;
  uint32_t override_endian = 2;
};

struct ResolveRequest {
  // Copy source (from RB_COPY_CONTROL) and destination (RB_COPY_DEST_*), read from the
  // register shadows after the D3D library prepared the resolve.
  int32_t src_x0 = 0, src_y0 = 0, src_x1 = -1, src_y1 = -1;  // -1: full surface
  int32_t dst_x = 0, dst_y = 0;
  bool has_dest_point = false;  // no point: the source rectangle's top-left (tiled resolves)
  bool has_dest = false;
  int32_t dest_format = -1;  // TextureFormat of the destination texture's fetch constant
  // Memory layout of the destination from the same fetch constant (the RB_COPY_DEST_INFO
  // shadow is not kept): endian swap and whether red and blue are swapped (swizzle ZYXW).
  int32_t dest_endian = -1;
  int32_t dest_red_blue_swap = -1;
  uint32_t flags = 0;  // D3DRESOLVE_* (bits 0-2 source, 0x100 clear color, 0x200 clear depth)
};

class Renderer {
 public:
  static Renderer& Get();

  void Draw(const GuestDevice& device, const DrawRequest& draw);
  // `before` = state when the resolve was requested, `after` = after the D3D library wrote
  // the copy registers.
  void Resolve(const GuestDevice& device, const ResolveRequest& request);
  // Clears the bound render targets (D3D Clear, BeginTiling). `color_targets` = bits 0-3,
  // `rects` = optional {x0, y0, x1, y1} list.
  void ClearBound(const GuestDevice& device, uint32_t color_targets, const float rgba[4],
                  bool depth, float z, bool stencil, uint8_t s, const int32_t* rects = nullptr,
                  uint32_t rect_count = 0);
  // Predicated tiling (BeginTiling .. EndTiling, including EndTiling's resolves): the GPU
  // replays the bracket once per tile with the surface sized to one tile. Draws are
  // executed once here, so the surface is the union of the tiles (`width` x `height`).
  void SetTiling(bool active, uint32_t width = 0, uint32_t height = 0);
  // `frontbuffer_fetch` = 6 big-endian dwords of the front buffer texture fetch constant.
  void Present(const uint8_t* frontbuffer_fetch);
  // The CPU is about to wait for `fence`: store the resolve results issued before it in guest
  // memory.
  void FlushWriteBacks(uint32_t fence);

  uint64_t frame() const;
  // COPA_TRACE window (see renderer.cpp) covers the current frame.
  bool tracing() const;
};

}  // namespace copa
