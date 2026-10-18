#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace copa::shader {

enum class Group { External, Literal, Masked };

struct Block {
  Group group;
  uint16_t offset;
  uint16_t count;
  size_t payload_offset;  // Byte offset from the beginning of T.
};

struct Metadata {
  uint64_t dirty_mask = 0;
  uint64_t dependency_mask = 0;
  std::vector<Block> blocks;
  std::string error;
  bool valid() const { return error.empty(); }
};

// Parses the table at T, never guest pointers or destination memory. The declared
// payload may end at an entry boundary in any group, without a final sentinel:
// both original bind functions stop when their cursor reaches the table end.
// Count-zero entries advance to the next group. The Masked sentinel terminates
// parsing; bytes after it are ignored, as in the original. Extra supplied bytes
// beyond the declared length are also ignored. Unlike the unchecked guest code,
// this reader rejects incomplete entries and odd masked counts. On failure the
// header and any preceding complete blocks remain available for diagnostics;
// callers must check valid() before treating the parse as complete.
inline Metadata ParseMetadata(std::span<const uint8_t> bytes) {
  Metadata result;
  if (bytes.size() < 20) {
    result.error = "truncated metadata header";
    return result;
  }
  const auto be16 = [&](size_t at) -> uint16_t {
    return static_cast<uint16_t>((uint16_t{bytes[at]} << 8) | bytes[at + 1]);
  };
  const auto be32 = [&](size_t at) -> uint32_t {
    return (uint32_t{bytes[at]} << 24) | (uint32_t{bytes[at + 1]} << 16) |
           (uint32_t{bytes[at + 2]} << 8) | bytes[at + 3];
  };
  result.dirty_mask = (uint64_t{be32(0)} << 32) | be32(4);
  result.dependency_mask = (uint64_t{be32(8)} << 32) | be32(12);
  const uint32_t length = be32(16);
  if (length > bytes.size() - 20) {
    result.error = "declared metadata payload exceeds input";
    return result;
  }
  const size_t end = 20 + static_cast<size_t>(length);
  size_t cursor = 20;
  Group group = Group::External;
  while (cursor < end) {
    if (end - cursor < 4) {
      result.error = "truncated block header";
      return result;
    }
    const uint16_t offset = be16(cursor);
    const uint16_t count = be16(cursor + 2);
    cursor += 4;
    if (count == 0) {
      if (group == Group::Masked) return result;
      group = group == Group::External ? Group::Literal : Group::Masked;
      continue;
    }
    if (group == Group::Masked && (count & 1)) {
      result.error = "masked block has odd word count";
      return result;
    }
    const uint64_t payload_size = group == Group::External ? 4 : uint64_t{count} * 4;
    // Literal counts are destination DWORDs; masked counts include two words
    // (AND, OR) per destination DWORD. External offsets are register indices,
    // not byte offsets into device+1152, so no destination-region inference.
    if (group != Group::External) {
      const uint64_t destination_size = group == Group::Literal ? payload_size : payload_size / 2;
      if (uint64_t{offset} + destination_size > std::numeric_limits<uint32_t>::max()) {
        result.error = "destination range exceeds 32 bits";
        return result;
      }
    }
    if (payload_size > end - cursor) {
      result.error = "truncated block payload";
      return result;
    }
    result.blocks.push_back({group, offset, count, cursor});
    cursor += static_cast<size_t>(payload_size);
  }
  return result;
}

}  // namespace copa::shader
