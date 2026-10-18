#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace copa::vertex {

// Imported source declaration, not a D3D input layout. Format and semantic
// enums remain encoded until a separately validated shader/format conversion.
struct SourceVertexElement {
  std::array<uint8_t, 12> source_bytes{};
  uint16_t stream = 0;
  uint16_t encoded_offset = 0;
  uint32_t encoded_format = 0;
  uint8_t encoded_semantic = 0;
  uint8_t encoded_semantic_index = 0;
  // +8 and +11 are deliberately available only through source_bytes: their
  // meanings are not established by the consumers examined so far.
};

struct SourceVertexLayout {
  std::vector<SourceVertexElement> elements;
  std::array<uint8_t, 16> stream_mask{};
  uint16_t highest_stream = 0;  // Matches original empty-list initialization.
  size_t consumed_bytes = 0;   // Includes only the two-byte terminator read.
};

struct ImportResult {
  std::optional<SourceVertexLayout> layout;
  std::string error;
  bool valid() const { return layout.has_value() && error.empty(); }
};

// Input is explicitly big-endian; no endianness guessing or guest-memory access.
// The original creator advances 12 bytes and recognizes BE16(stream)==255.
// 16 streams follows its 16-byte mask, not an assumed graphics API limit.
// max_elements is a host allocation policy, not a recovered source limit.
inline ImportResult ImportSourceVertexLayout(std::span<const uint8_t> bytes,
                                            size_t max_elements = 4096) {
  const auto fail = [](const char* message) {
    return ImportResult{std::nullopt, message};
  };
  const auto read16 = [&](size_t at) -> uint16_t {
    return static_cast<uint16_t>((uint16_t{bytes[at]} << 8) | bytes[at + 1]);
  };
  SourceVertexLayout candidate;
  size_t at = 0;
  for (;;) {
    if (bytes.size() - at < 2) return fail("missing or truncated stream terminator");
    const uint16_t stream = read16(at);
    if (stream == 255) {
      candidate.consumed_bytes = at + 2;
      return {std::move(candidate), {}};
    }
    if (stream >= candidate.stream_mask.size())
      return fail("stream is outside the recovered 16-stream mask");
    if (bytes.size() - at < 12) return fail("truncated vertex element");
    if (candidate.elements.size() >= max_elements)
      return fail("vertex element count exceeds host import limit");
    SourceVertexElement element;
    std::copy_n(bytes.begin() + at, 12, element.source_bytes.begin());
    element.stream = stream;
    element.encoded_offset = read16(at + 2);
    element.encoded_format = (uint32_t{bytes[at + 4]} << 24) |
                            (uint32_t{bytes[at + 5]} << 16) |
                            (uint32_t{bytes[at + 6]} << 8) | bytes[at + 7];
    element.encoded_semantic = bytes[at + 9];
    element.encoded_semantic_index = bytes[at + 10];
    candidate.elements.push_back(element);
    candidate.stream_mask[stream] = 255;
    candidate.highest_stream = std::max(candidate.highest_stream, stream);
    at += 12;
  }
}

}  // namespace copa::vertex
