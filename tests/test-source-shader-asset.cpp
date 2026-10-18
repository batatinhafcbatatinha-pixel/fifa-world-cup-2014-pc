#include "../project/src/source_shader_asset.h"

#include <algorithm>
#include <cassert>
#include <iostream>

using namespace copa::shader;
using Bytes = std::vector<uint8_t>;

static void Put(Bytes& bytes, size_t at, uint32_t value) {
  assert(at + 4 <= bytes.size());
  for (size_t i = 0; i < 4; ++i)
    bytes[at + i] = static_cast<uint8_t>(value >> (24 - 8 * i));
}

static Bytes Fixture(bool metadata = true) {
  // Header has padding after the 28-byte table to test the exact span.
  Bytes blob(80 + 12);
  Put(blob, 0, 0xa1b2c3d4);
  Put(blob, 4, 80);
  Put(blob, 8, 12);
  Put(blob, 20, metadata ? 32 : 0);
  Put(blob, 32, 0x01234567); Put(blob, 36, 0x89abcdef);
  Put(blob, 40, 0xfedcba98); Put(blob, 44, 0x76543210);
  Put(blob, 48, 8); // External entry ends legally without a sentinel.
  Put(blob, 52, 0x8123f002); // dest kind=2,index=0x48c; masked count=2.
  Put(blob, 56, 4); // Body-relative byte offset.
  std::fill(blob.begin() + 60, blob.begin() + 80, 0xee);
  Put(blob, 80, 0x11223344); Put(blob, 84, 0x55667788); Put(blob, 88, 0x99aabbcc);
  return blob;
}

static void Reject(const Bytes& bytes) {
  const auto result = ImportSourceShader(bytes, Stage::Vertex);
  assert(!result.valid() && !result.asset && !result.error.empty());
}

int main() {
  Bytes blob = Fixture();
  auto result = ImportSourceShader(blob, Stage::Pixel);
  assert(result.valid());
  assert(result.asset->stage == Stage::Pixel);
  assert(result.asset->source_flags == 0xa1b2c3d4);
  assert(result.asset->header.size() == 80 && result.asset->body.size() == 12);
  assert(result.asset->metadata_offset == 32 && result.asset->metadata_length == 28);
  assert(result.asset->metadata_bytes().size() == 28);
  assert(result.asset->metadata_bytes().data() == result.asset->header.data() + 32);
  assert(result.asset->metadata.dirty_mask == 0x0123456789abcdefULL);
  assert(result.asset->metadata.dependency_mask == 0xfedcba9876543210ULL);
  assert(result.asset->external_constants.size() == 1);
  const auto external = result.asset->external_constants.front();
  assert(external.encoded_destination == 0x8123 && external.encoded_count == 0xf002);
  assert(external.destination_kind == 2 && external.destination_dword == 0x48c);
  assert(external.dword_count == 2 && external.body_offset == 4);
  assert(result.asset->body[4] == 0x55 && result.asset->body[7] == 0x88);
  const Bytes expected_header = result.asset->header;
  const Bytes expected_body = result.asset->body;
  std::fill(blob.begin(), blob.end(), 0);
  blob.clear(); blob.shrink_to_fit();
  SourceShaderAsset moved = std::move(*result.asset);
  result.asset.reset();
  assert(moved.header == expected_header && moved.body == expected_body);
  assert(moved.metadata_bytes().data() == moved.header.data() + 32);
  assert(moved.metadata_bytes().size() == 28 && moved.metadata_bytes().back() == 4);
  assert(moved.metadata.blocks.front().payload_offset == 24);

  const auto absent = ImportSourceShader(Fixture(false), Stage::Vertex);
  assert(absent.valid() && absent.asset->stage == Stage::Vertex);
  assert(!absent.asset->metadata_offset && absent.asset->metadata_bytes().empty());
  assert(absent.asset->external_constants.empty() && absent.asset->metadata.valid());
  Bytes empty(24); Put(empty, 4, 24);
  assert(ImportSourceShader(empty, Stage::Pixel).valid());
  Bytes table_empty = Fixture(); Put(table_empty, 48, 0);
  auto empty_result = ImportSourceShader(table_empty, Stage::Pixel);
  assert(empty_result.valid() && empty_result.asset->metadata_bytes().size() == 20);
  assert(empty_result.asset->external_constants.empty());

  const Bytes full = Fixture();
  for (size_t n = 0; n < full.size(); ++n)
    Reject(Bytes(full.begin(), full.begin() + n));
  for (uint32_t header_length : {0u, 23u, 93u, 0xffffffffu}) {
    Bytes bad = full; Put(bad, 4, header_length); Reject(bad);
  }
  for (uint32_t body_length : {13u, 0xffffffffu}) {
    Bytes bad = full; Put(bad, 8, body_length); Reject(bad);
  }
  for (uint32_t offset : {61u, 80u, 81u, 0xffffffffu}) {
    Bytes bad = full; Put(bad, 20, offset); Reject(bad);
  }
  for (uint32_t length : {1u, 3u, 4u, 7u, 29u, 0xffffffffu}) {
    Bytes bad = full; Put(bad, 48, length); Reject(bad);
  }
  for (uint32_t offset : {5u, 12u, 0xffffffffu}) {
    Bytes bad = full; Put(bad, 56, offset); Reject(bad);
  }
  Bytes zero_count = full;
  Put(zero_count, 52, 0x8123f000); Put(zero_count, 56, 12);
  auto zero = ImportSourceShader(zero_count, Stage::Vertex);
  assert(zero.valid() && zero.asset->external_constants.front().dword_count == 0);
  Put(zero_count, 56, 13); Reject(zero_count);
  Bytes extra = full; extra.insert(extra.end(), 9, 0xff);
  assert(ImportSourceShader(extra, Stage::Vertex).asset->body == expected_body);

  // Public assets can be mutated; span accessor must still avoid invalid ranges.
  moved.metadata_offset = moved.header.size() + 1;
  assert(moved.metadata_bytes().empty());
  moved.metadata_offset = 32; moved.metadata_length = 49;
  assert(moved.metadata_bytes().empty());
  std::cout << "source shader asset tests passed\n";
}
