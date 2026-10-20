// Xenos shader microcode -> HLSL (Shader Model 5) translator for the native renderer.
//
// Input: the microcode the game's D3D library would upload with IM_LOAD (host-endian
// dwords). Output: HLSL source compiled by d3dcompiler at run time and cached on disk;
// the game compiles part of its shaders at run time with its own HLSL compiler, so an
// ahead-of-time-only pass could not cover them.
//
// Binding contract with the renderer (renderer.cpp):
//   b0  float4 c[256]           vertex shader float constants (little-endian)
//   b1  float4 c[256]           pixel shader float constants
//   b2  SystemConstants         bools, loops, fetch info, viewport, alpha test
//   t0..t31   textures by fetch slot (type chosen by the tfetch dimension)
//   s0..s31   samplers by fetch slot
//   t32..     raw vertex buffers, one per used vertex fetch constant (see vfetch_slots)
//   t96..t127 pixel shader 3D textures as volumes (tex_flags bit 1 selects them over t0..t31)

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace copa {

enum class ShaderStage : uint8_t { kVertex, kPixel };

constexpr uint32_t kVolumeTextureBase = 96;

// Draw-time state that changes the generated code.
struct ShaderKey {
  // Pixel shader: SQ_PROGRAM_CNTL.param_gen and SQ_CONTEXT_MISC.param_gen_pos.
  bool param_gen = false;
  uint8_t param_gen_register = 0;
  // Vertex shader: primitive is a rectangle list (expanded by a geometry shader).
  bool rect_list = false;

  uint64_t Pack() const {
    return uint64_t(param_gen) | (uint64_t(param_gen_register) << 1) | (uint64_t(rect_list) << 9);
  }
};

enum class TextureDimension : uint8_t { kNone, k1D, k2D, k3D, kStacked, kCube };

struct TranslatedShader {
  bool ok = false;
  std::string error;
  std::string hlsl;
  ShaderStage stage = ShaderStage::kVertex;
  // Vertex fetch constant indices (0-95) read by vfetch, in t32+ binding order.
  std::vector<uint8_t> vfetch_slots;
  // Per binding: vertex stride and the end of the furthest element read (dwords).
  std::vector<uint32_t> vfetch_stride, vfetch_end;
  // Texture fetch slots used and the resource type each needs.
  uint32_t tfetch_mask = 0;
  std::array<TextureDimension, 32> tfetch_dimension{};
  // Pixel shader: render targets written (bits 0-3) and depth output.
  uint32_t color_outputs = 0;
  bool writes_depth = false;
  bool uses_kill = false;
  // Writes memory through eA/eM# (memexport), which is not translated.
  bool uses_memexport = false;
  // Vertex shader: interpolators written (bits 0-15).
  uint32_t interpolators_written = 0;
};

// `dwords` are host-endian; `count` is the microcode size in dwords.
TranslatedShader TranslateShader(ShaderStage stage, const uint32_t* dwords, uint32_t count,
                                 const ShaderKey& key);

// HLSL shared by all generated shaders (declarations of the binding contract).
const char* ShaderCommonHlsl();

// Layout of b2, kept in sync with ShaderCommonHlsl().
struct alignas(16) SystemConstants {
  uint32_t bools[8];             // 256 boolean constants
  uint32_t loops[32];            // loop constants (count, start, step)
  uint32_t vfetch_endian[96];    // endian mode per vertex fetch constant
  uint32_t tex_swizzle[32];      // fetch constant swizzle (3 bits per component)
  uint32_t tex_flags[32];        // bit0 biased sign, bit1 volume, bits 8..15 exp_adjust (signed)
  float tex_size[32][4];         // width, height, depth, 0 (for unnormalized coords)
  float viewport[4];             // xscale, xoffset, yscale, yoffset (applied in VS)
  float viewport_z[4];           // zscale, zoffset, rt_width, rt_height
  uint32_t vte_flags;            // PA_CL_VTE_CNTL
  float pixel_offset;            // 0.5 when PA_SU_VTX_CNTL pixel center is D3D9-style
  float window_offset_x, window_offset_y;
  uint32_t alpha_test;           // bit 3 enable, bits 0-2 function
  float alpha_ref;
  float res_scale[2];            // internal resolution: host pixels per guest pixel, inverse
  float rt_exp_scale[4];         // 2^color_exp_bias for each render target
};

}  // namespace copa
