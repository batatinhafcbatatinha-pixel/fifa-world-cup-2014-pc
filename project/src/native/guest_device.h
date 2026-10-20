// Layout of the game's Direct3D device (the XDK D3D library statically linked into
// default.xex, around 0x82440000-0x82470000). It is the same library build as FIFA Street
// (2012); fields below +0x2D20 match it, later ones are shifted by +0x18 (up to +0x34EC)
// or +0x10 (see NATIVE_PLAN.md). All values are big-endian in guest memory.
//
// The D3D library keeps the full pipeline state in "register shadows": the values it
// would write to Xenos context registers. The native renderer reads these shadows at
// draw time; it never reads the command buffer.

#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include <rex/memory.h>

namespace copa {

inline uint32_t LoadBE32(const void* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return rex::byte_swap(v);
}
inline uint16_t LoadBE16(const void* p) {
  uint16_t v;
  std::memcpy(&v, p, 2);
  return rex::byte_swap(v);
}

// Host pointer of a guest virtual address. The 0xE0000000+ physical view is mapped with a
// 4 KB host offset (same rule as REX_PHYS_HOST_OFFSET in the generated code).
inline uint8_t* GuestPtr(uint8_t* base, uint32_t address) {
  return base + address + (address >= 0xE0000000u ? 0x1000u : 0u);
}

// Plausible guest object/data pointer (not null, not in the first 64 KB).
inline bool ValidGuest(uint32_t address) { return address >= 0x10000u; }

namespace dev {
// Ring position low bits (timestamp insertion).
constexpr uint32_t kRingPositionBits = 0x3BDC;
// Dirty masks (5 x u64) at +0x00, ring write pointer at +0x30, ring limit at +0x38.
constexpr uint32_t kRingWrite = 0x30;
constexpr uint32_t kRingLimit = 0x38;
// 32 fetch constant groups of 6 dwords (registers 0x4800+).
constexpr uint32_t kFetchConstants = 0x480;
// 256 float4 vertex shader constants (registers 0x4000+).
constexpr uint32_t kVsConstants = 0x780;
// 256 float4 pixel shader constants (registers 0x4400+).
constexpr uint32_t kPsConstants = 0x1780;
// Boolean constants (8 dwords) followed by 32 loop constants (registers 0x4900+).
constexpr uint32_t kBoolLoopConstants = 0x2780;
// GPU status blocks written by the CP: +0x2B10 -> [0] timestamp, [4] ring position;
// +0x2B14 -> callback function/argument at +0x10/+0x14 for CP interrupts.
constexpr uint32_t kStatusBlock = 0x2B10;
constexpr uint32_t kCallbackBlock = 0x2B14;
constexpr uint32_t kTimestamp = 0x2B1C;
constexpr uint32_t kFlags2B3C = 0x2B3C;  // bit 0x01: predicated tiling active
constexpr uint32_t kVertexDeclaration = 0x2FD0;
constexpr uint32_t kIndexBuffer = 0x320C;   // D3DIndexBuffer*: +0 flags (bit31 32-bit), +0x18 address
constexpr uint32_t kPixelShader = 0x330C;   // D3DPixelShader*
constexpr uint32_t kVertexShader = 0x3310;  // D3DVertexShader*
constexpr uint32_t kInlineVertexData = 0x3614;
}  // namespace dev

// Register shadow blocks: register range -> device offset.
struct ShadowBlock {
  uint32_t first_register, count, offset;
};
constexpr ShadowBlock kShadowBlocks[] = {
    {0x2000, 16, 0x2880},  // RB_SURFACE_INFO .. PA_SC_SCREEN_SCISSOR_BR
    {0x2080, 3, 0x28C0},   // PA_SC_WINDOW_OFFSET .. PA_SC_WINDOW_SCISSOR_BR
    {0x2100, 21, 0x28CC},  // VGT_MAX_VTX_INDX .. PA_CL_VPORT_ZOFFSET
    {0x2180, 5, 0x2920},   // SQ_PROGRAM_CNTL .. SQ_WRAPPING_1
    {0x2200, 12, 0x2934},  // RB_DEPTHCONTROL .. RB_BLENDCONTROL3
    {0x2280, 21, 0x2964},  // PA_SU_POINT_SIZE .. VGT_ENHANCE
    {0x2300, 38, 0x29B8},  // PA_SC_LINE_CNTL .. RB_SAMPLE_COUNT_ADDR
    {0x2380, 8, 0x2A50},   // PA_SU_POLY_OFFSET_* .. PA_CL_POINT_CULL_RAD
    {0x2388, 24, 0x2A70},  // PA_CL_UCP_0..5
};

constexpr int32_t ShadowOffset(uint32_t reg) {
  for (const auto& b : kShadowBlocks)
    if (reg >= b.first_register && reg < b.first_register + b.count)
      return int32_t(b.offset + 4 * (reg - b.first_register));
  return -1;
}

// Read-only view of the guest device at a given moment.
class GuestDevice {
 public:
  GuestDevice(uint8_t* membase, uint32_t address) : base_(membase), address_(address) {}

  uint32_t address() const { return address_; }
  const uint8_t* ptr(uint32_t offset) const { return GuestPtr(base_, address_ + offset); }
  uint32_t u32(uint32_t offset) const { return LoadBE32(ptr(offset)); }
  uint8_t u8(uint32_t offset) const { return *ptr(offset); }

  uint32_t reg(uint32_t index) const {
    int32_t offset = ShadowOffset(index);
    return offset < 0 ? 0 : u32(uint32_t(offset));
  }
  float reg_float(uint32_t index) const {
    uint32_t v = reg(index);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  }
  // 6 big-endian dwords of texture fetch constant `slot` (0-31).
  const uint8_t* fetch_constant(uint32_t slot) const { return ptr(dev::kFetchConstants + slot * 24); }
  // 2 dwords of vertex fetch constant `index` (0-95).
  const uint8_t* vertex_fetch_constant(uint32_t index) const {
    return ptr(dev::kFetchConstants + index * 8);
  }
  const uint8_t* vs_constants() const { return ptr(dev::kVsConstants); }
  const uint8_t* ps_constants() const { return ptr(dev::kPsConstants); }
  const uint8_t* bool_loop_constants() const { return ptr(dev::kBoolLoopConstants); }
  bool tiling_active() const { return (u8(dev::kFlags2B3C) & 1) != 0; }

 private:
  uint8_t* base_;
  uint32_t address_;
};

// Microcode location of a D3D shader object (big-endian dwords, guest virtual address).
struct ShaderMicrocode {
  uint32_t address = 0;  // guest virtual address
  uint32_t dword_count = 0;
  const uint8_t* host = nullptr;  // when set, the microcode is here instead (big-endian too)
};

// The flush patches a binding's vfetch instructions in place (0x82457498) only when the GPU
// is past the binding's last use; otherwise (0x82457290) it copies the microcode into the
// command buffer (IM_LOAD_IMMEDIATE), patches the copy and leaves memory patched for the
// previous vertex declaration. The copy stays the loaded shader until the next load; the
// hooks keep it here (d3d_hooks.cpp) and VertexShaderMicrocode returns it.
struct ImmediateVertexShader {
  uint32_t shader = 0, binding = 0;
  std::vector<uint8_t> code;
};
inline ImmediateVertexShader& LoadedImmediateVertexShader() {
  static ImmediateVertexShader s;
  return s;
}

// D3DVertexShader: microcode base at +0x20 (physical), binding table at +0x380 (8 bytes
// per binding; first word = offset of the binding record relative to the shader).
// Binding record +0x368 = microcode offset, +0x36C = size in bytes. Binding 1 is used
// when (shader+0x368) & 0x20. The D3D flush (0x824575D8) patches the vfetch
// instructions of the selected binding in place for the current vertex declaration.
inline ShaderMicrocode VertexShaderMicrocode(uint8_t* membase, uint32_t shader) {
  ShaderMicrocode m;
  if (!ValidGuest(shader)) return m;
  const uint8_t* s = GuestPtr(membase, shader);
  uint32_t binding = (LoadBE32(s + 0x368) & 0x20) ? 1 : 0;
  uint32_t record = shader + LoadBE32(s + 0x380 + 8 * binding);
  if (!ValidGuest(record) || !ValidGuest(LoadBE32(s + 0x20))) return m;
  m.address = LoadBE32(s + 0x20) + LoadBE32(GuestPtr(membase, record + 0x368));
  m.dword_count = LoadBE32(GuestPtr(membase, record + 0x36C)) / 4;
  const auto& imm = LoadedImmediateVertexShader();
  if (imm.shader == shader && imm.binding == binding && imm.code.size() == size_t(m.dword_count) * 4)
    m.host = imm.code.data();
  return m;
}

// D3DPixelShader: microcode base at +0x18, record offset at +0x40; record +0x28 =
// microcode offset, +0x2C = size in bytes.
inline ShaderMicrocode PixelShaderMicrocode(uint8_t* membase, uint32_t shader) {
  ShaderMicrocode m;
  if (!ValidGuest(shader)) return m;
  const uint8_t* s = GuestPtr(membase, shader);
  uint32_t record = shader + LoadBE32(s + 0x40);
  if (!ValidGuest(record) || !ValidGuest(LoadBE32(s + 0x18))) return m;
  m.address = LoadBE32(s + 0x18) + LoadBE32(GuestPtr(membase, record + 0x28));
  m.dword_count = LoadBE32(GuestPtr(membase, record + 0x2C)) / 4;
  return m;
}

}  // namespace copa
