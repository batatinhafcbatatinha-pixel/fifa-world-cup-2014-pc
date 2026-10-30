#pragma once
#include <cstdint>
#include <vector>
namespace pcs { struct ThemeFont; }
namespace native_host {
void RasterizeFont(const std::vector<uint8_t>& data, int pixels, pcs::ThemeFont& output);
}
