#include "../project/src/source_vertex_layout.h"

#include <cassert>
#include <iostream>

using namespace copa::vertex;
using Bytes = std::vector<uint8_t>;

int main() {
  const Bytes empty{0, 255};
  auto result = ImportSourceVertexLayout(empty, 0);
  assert(result.valid() && result.layout->elements.empty());
  assert(result.layout->highest_stream == 0 && result.layout->consumed_bytes == 2);
  assert((result.layout->stream_mask == std::array<uint8_t, 16>{}));

  Bytes bytes{0, 15, 0x12, 0x34, 0x89, 0xab, 0xcd, 0xef, 0x71, 3, 5, 0x82,
              0, 2, 0xff, 0xfe, 1, 2, 3, 4, 0x91, 3, 6, 0xa2,
              0, 15, 0, 4, 0, 0, 0, 0, 0, 3, 7, 0,
              0, 255, 0xfa, 0xfb};
  const auto original = bytes;
  result = ImportSourceVertexLayout(bytes);
  assert(result.valid());
  auto layout = std::move(*result.layout);
  bytes.assign(bytes.size(), 0);
  assert(layout.elements.size() == 3 && layout.highest_stream == 15);
  assert(layout.consumed_bytes == 38);
  for (size_t i = 0; i < layout.elements.size(); ++i)
    for (size_t j = 0; j < 12; ++j)
      assert(layout.elements[i].source_bytes[j] == original[i * 12 + j]);
  assert(layout.elements[0].encoded_offset == 0x1234);
  assert(layout.elements[0].encoded_format == 0x89abcdef);
  assert(layout.elements[0].encoded_semantic == 3);
  assert(layout.elements[0].encoded_semantic_index == 5);
  for (size_t i = 0; i < 16; ++i)
    assert(layout.stream_mask[i] == ((i == 2 || i == 15) ? 255 : 0));
  // Every prefix before the complete terminator must reject, without a partial asset.
  for (size_t n = 0; n < 38; ++n) {
    const auto truncated = ImportSourceVertexLayout(std::span(original).first(n));
    assert(!truncated.valid() && !truncated.layout && !truncated.error.empty());
  }
  assert(!ImportSourceVertexLayout(original, 2).valid());
  assert(ImportSourceVertexLayout(original, 3).valid());
  for (const uint16_t stream : {uint16_t{16}, uint16_t{254}, uint16_t{256}, uint16_t{65535}}) {
    auto invalid = original;
    invalid[0] = static_cast<uint8_t>(stream >> 8);
    invalid[1] = static_cast<uint8_t>(stream);
    assert(!ImportSourceVertexLayout(invalid).valid());
  }
  // A little-endian stream 15 and little-endian terminator are not accepted.
  auto swapped = original;
  std::swap(swapped[0], swapped[1]);
  assert(!ImportSourceVertexLayout(swapped).valid());
  assert(!ImportSourceVertexLayout(Bytes{255, 0}).valid());
  std::cout << "source vertex layout: ownership, endian, stream mask, limits and truncation passed\n";
}
