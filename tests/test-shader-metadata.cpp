#include "../project/src/shader_metadata.h"

#include <cassert>
#include <iostream>

using copa::shader::Group;
using copa::shader::ParseMetadata;
using Bytes = std::vector<uint8_t>;

static void Word(Bytes& out, uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8)
    out.push_back(static_cast<uint8_t>(value >> shift));
}
static void Entry(Bytes& out, uint16_t offset, uint16_t count) {
  Word(out, (uint32_t{offset} << 16) | count);
}
static Bytes Table(const Bytes& payload) {
  Bytes out;
  Word(out, 0x01234567); Word(out, 0x89abcdef);
  Word(out, 0xfedcba98); Word(out, 0x76543210);
  Word(out, static_cast<uint32_t>(payload.size()));
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

int main() {
  Bytes payload;
  Entry(payload, 0x1234, 7); Word(payload, 0x10203040); // Always one external word.
  Entry(payload, 0xffff, 0); // Offset is immaterial for a sentinel.
  Entry(payload, 24, 2); Word(payload, 0x11223344); Word(payload, 0x55667788);
  Entry(payload, 4864, 1); Word(payload, 0xaabbccdd);
  Entry(payload, 0, 0);
  Entry(payload, 48, 4); Word(payload, 0xfffffffc); Word(payload, 2);
  Word(payload, 0xff00ff00); Word(payload, 0x00010001);
  Entry(payload, 0, 0);
  const Bytes full = Table(payload);
  auto result = ParseMetadata(full);
  assert(result.valid() && result.blocks.size() == 4);
  assert(result.dirty_mask == 0x0123456789abcdefULL);
  assert(result.dependency_mask == 0xfedcba9876543210ULL);
  assert(result.blocks[0].group == Group::External && result.blocks[0].count == 7);
  assert(result.blocks[0].offset == 0x1234 && result.blocks[0].payload_offset == 24);
  assert(result.blocks[1].group == Group::Literal && result.blocks[1].offset == 24);
  assert(result.blocks[1].payload_offset == 36 && result.blocks[1].count == 2);
  assert(result.blocks[2].offset == 4864);
  assert(result.blocks[3].group == Group::Masked && result.blocks[3].count == 4);
  assert(result.blocks[3].payload_offset == 60);
  // Diagnostic classification only: the parser never applies these writes.
  size_t fetch_overlaps = 0;
  for (const auto& block : result.blocks)
    if (block.group != Group::External && block.offset < 26 * 24) ++fetch_overlaps;
  assert(fetch_overlaps == 2);

  // Every prefix is rejected while the original declared length is retained.
  for (size_t n = 0; n < full.size(); ++n)
    assert(!ParseMetadata(std::span(full).first(n)).valid());
  for (size_t n = 1; n < 4; ++n) assert(!ParseMetadata(Table(Bytes(n))).valid());
  for (const Group group : {Group::External, Group::Literal, Group::Masked}) {
    Bytes prefix;
    if (group != Group::External) Entry(prefix, 0, 0);
    if (group == Group::Masked) Entry(prefix, 0, 0);
    Bytes incomplete = prefix;
    Entry(incomplete, 0, 2);
    assert(!ParseMetadata(Table(incomplete)).valid());
    Word(incomplete, 0);
    if (group != Group::External) assert(!ParseMetadata(Table(incomplete)).valid());
    else assert(ParseMetadata(Table(incomplete)).valid());
    if (group != Group::External) {
      Word(incomplete, 0);
      assert(ParseMetadata(Table(incomplete)).valid()); // End without sentinel.
    }
    assert(ParseMetadata(Table(prefix)).valid());
  }
  Bytes odd; Entry(odd, 0, 0); Entry(odd, 0, 0); Entry(odd, 0, 3);
  Word(odd, 0); Word(odd, 0); Word(odd, 0);
  assert(!ParseMetadata(Table(odd)).valid());
  // A large byte offset is allowed; its destination type is unknown.
  Bytes large; Entry(large, 0, 0); Entry(large, 65535, 1); Word(large, 0);
  assert(ParseMetadata(Table(large)).valid());
  Bytes tail = payload; tail.push_back(0xff);
  assert(ParseMetadata(Table(tail)).valid()); // Masked sentinel ends parsing.
  Bytes extra = full; extra.push_back(0xff);
  assert(ParseMetadata(extra).valid()); // Outside declared table length.
  Bytes excessive = Table({});
  excessive[16] = excessive[17] = excessive[18] = excessive[19] = 0xff;
  assert(!ParseMetadata(excessive).valid());
  assert(ParseMetadata(Table({})).valid());
  std::cout << "shader metadata tests passed\n";
}
