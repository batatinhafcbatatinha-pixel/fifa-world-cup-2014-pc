#pragma once
#include <bit>
#include <cstdint>
#include "../pc_settings.h"

// Post-processing switches (same policy as Street 4). Copa's final brightness/contrast pass is
// the same pixel shader as Street 4's (identical microcode hash). Its vignette lives in a
// different composite shader that was not identified, so it has no switch.
namespace ea_postfx {
enum Effect { BrightnessContrast, Count };
enum class Pass { Other, BrightnessContrast };
inline Pass Identify(uint64_t pixel_shader_hash) {
  return pixel_shader_hash == 0xa392657905ecd8c0ull ? Pass::BrightnessContrast : Pass::Other;
}
inline bool Enabled(Pass pass) {
  return pass != Pass::BrightnessContrast || pcs::EffectEnabled(BrightnessContrast);
}
// Host-endian PS float constants, after literal overlay. Keep the original draw: it copies the
// current scene into its output; skipping it would leave stale data.
inline void Neutralize(Pass pass, uint32_t* ps) {
  if (pass == Pass::BrightnessContrast) {
    // Output = scene * c0.y + c0.x. Identity preserves the alpha/depth contract.
    ps[0] = 0;
    ps[1] = std::bit_cast<uint32_t>(1.0f);
  }
}
}
