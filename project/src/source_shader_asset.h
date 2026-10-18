#pragma once

#include "shader_metadata.h"

#include <optional>
#include <utility>

namespace copa::shader {

enum class Stage { Vertex, Pixel };

// These are imported source assets, NOT executable host GPU shaders. A later
// offline shader compiler must replace the source program with native shader
// code. No Xbox resource object, guest pointer, MMIO or PM4 stream is retained.
struct ExternalConstantSource {
  uint16_t encoded_destination = 0;
  uint16_t encoded_count = 0;
  uint32_t body_offset = 0;
  uint32_t destination_kind = 0;
  uint32_t destination_dword = 0;
  uint32_t dword_count = 0;
};

struct SourceShaderAsset {
  Stage stage = Stage::Vertex;
  uint32_t source_flags = 0;
  std::vector<uint8_t> header;
  std::vector<uint8_t> body;
  std::optional<size_t> metadata_offset;
  size_t metadata_length = 0;
  Metadata metadata;
  std::vector<ExternalConstantSource> external_constants;

  std::span<const uint8_t> metadata_bytes() const {
    if (!metadata_offset || *metadata_offset > header.size() ||
        metadata_length > header.size() - *metadata_offset) return {};
    return std::span<const uint8_t>(header).subspan(*metadata_offset, metadata_length);
  }
};

struct ImportResult {
  std::optional<SourceShaderAsset> asset;
  std::string error;
  bool valid() const { return asset.has_value() && error.empty(); }
};

// Source layout from 8245BF60 / 8245C148: BE32(+4) is header length,
// BE32(+8) is body length, body follows header. Stage comes from the creation
// entry point, never guessed from the filename or the opaque flags word.
// The bind routines read metadata relative to the copied header at BE32(+20).
inline ImportResult ImportSourceShader(std::span<const uint8_t> blob, Stage stage) {
  const auto fail = [](std::string message) {
    return ImportResult{std::nullopt, std::move(message)};
  };
  if (blob.size() < 24) return fail("truncated source shader header");
  const auto read32 = [](std::span<const uint8_t> bytes, size_t at) -> uint32_t {
    return (uint32_t{bytes[at]} << 24) | (uint32_t{bytes[at + 1]} << 16) |
           (uint32_t{bytes[at + 2]} << 8) | bytes[at + 3];
  };
  const size_t header_size = read32(blob, 4);
  const size_t body_size = read32(blob, 8);
  if (header_size < 24 || header_size > blob.size())
    return fail("source shader header length is outside input");
  if (body_size > blob.size() - header_size)
    return fail("source shader body length is outside input");

  SourceShaderAsset candidate;
  candidate.stage = stage;
  candidate.source_flags = read32(blob, 0);
  const auto header = blob.first(header_size);
  const auto body = blob.subspan(header_size, body_size);
  const size_t metadata_offset = read32(header, 20);
  if (metadata_offset != 0) {
    if (metadata_offset > header.size() || header.size() - metadata_offset < 20)
      return fail("source shader metadata header is outside shader header");
    const auto table = header.subspan(metadata_offset);
    candidate.metadata = ParseMetadata(table);
    if (!candidate.metadata.valid())
      return fail("source shader metadata: " + candidate.metadata.error);
    candidate.metadata_offset = metadata_offset;
    candidate.metadata_length = 20 + static_cast<size_t>(read32(table, 16));
    for (const auto& block : candidate.metadata.blocks) {
      if (block.group != Group::External) continue;
      // ParseMetadata has already proved this four-byte payload is present.
      const uint32_t body_offset = read32(table, block.payload_offset);
      // 824573E8 encodes destination<<2 and count. The static source consumer
      // defines an 11-bit index, an 8-bit class and a 12-bit DWORD count.
      // Normalize those asset fields here; do not execute an encoded packet.
      const uint32_t destination = uint32_t{block.offset} << 2;
      const uint32_t count = block.count & 0xFFF;
      const uint64_t source_end = uint64_t{body_offset} + uint64_t{count} * 4;
      if (source_end > body.size())
        return fail("external shader constants extend beyond source body");
      // A future native constant-buffer consumer must separately validate the
      // destination class and capacity. This importer performs no writes.
      candidate.external_constants.push_back({block.offset, block.count,
          body_offset, (destination >> 16) & 0xFF, destination & 0x7FF, count});
    }
  }
  // Own all data. Metadata offsets are offsets, never pointers into the
  // caller's memory, so moving the result cannot invalidate them.
  candidate.header.assign(header.begin(), header.end());
  candidate.body.assign(body.begin(), body.end());
  return ImportResult{std::move(candidate), {}};
}

}  // namespace copa::shader
