#pragma once

#include "source_shader_asset.h"

namespace copa::shader {

// Host DWORDs contain numeric values, not big-endian byte representations.
// The caller supplies destination semantics and capacities; no device layout
// is inferred here. Classes must be unique, and mapped host spans disjoint.
struct ConstantDestination {
  uint32_t kind;
  std::span<uint32_t> words;
};
struct ParameterRegion {
  uint32_t byte_offset;
  std::span<uint32_t> words;
};
struct ParameterUpdateResult {
  std::string error;
  bool valid() const { return error.empty(); }
};

namespace parameter_detail {
struct Write {
  uint32_t* destination;
  uint32_t and_mask;
  uint32_t or_mask;
};
inline uint32_t Read(std::span<const uint8_t> bytes, size_t at) {
  return (uint32_t{bytes[at]} << 24) | (uint32_t{bytes[at + 1]} << 16) |
         (uint32_t{bytes[at + 2]} << 8) | bytes[at + 3];
}
inline bool Overlap(std::span<uint32_t> a, std::span<uint32_t> b) {
  if (a.empty() || b.empty()) return false;
  const auto first = reinterpret_cast<uintptr_t>(a.data());
  const auto second = reinterpret_cast<uintptr_t>(b.data());
  return first <= second ? second - first < a.size_bytes()
                         : first - second < b.size_bytes();
}
inline void Commit(const std::vector<Write>& writes) {
  for (const auto& write : writes)
    *write.destination = write.and_mask == 0 ? write.or_mask :
        (*write.destination & write.and_mask) | write.or_mask;
}
}  // namespace parameter_detail

// Draw-time external copies and bind-time patches are separate operations:
// this API deliberately establishes no ordering between those two phases.
// Each call validates and snapshots every source word before any host write.
// Callers must supply valid spans and exclusive access during a call.
// Failure leaves all destinations untouched. Writes within a call retain input
// order, including overlapping writes to a single destination mapping.
inline ParameterUpdateResult ApplyExternalConstants(
    std::span<const uint8_t> body,
    std::span<const ExternalConstantSource> sources,
    std::span<const ConstantDestination> destinations) {
  for (size_t i = 0; i < destinations.size(); ++i) {
    for (size_t j = 0; j < i; ++j) {
      if (destinations[i].kind == destinations[j].kind)
        return {"duplicate constant destination class"};
      if (parameter_detail::Overlap(destinations[i].words, destinations[j].words))
        return {"overlapping host constant destinations"};
    }
  }
  std::vector<parameter_detail::Write> writes;
  for (const auto& source : sources) {
    const ConstantDestination* target = nullptr;
    for (const auto& destination : destinations)
      if (destination.kind == source.destination_kind) target = &destination;
    if (!target) return {"unmapped constant destination class"};
    if (source.body_offset % 4 != 0) return {"unaligned constant source"};
    if (source.body_offset > body.size() ||
        source.dword_count > (body.size() - source.body_offset) / 4)
      return {"constant source outside body"};
    if (source.destination_dword > target->words.size() ||
        source.dword_count > target->words.size() - source.destination_dword)
      return {"constant destination outside capacity"};
    for (size_t i = 0; i < source.dword_count; ++i)
      writes.push_back({&target->words[source.destination_dword + i], 0,
          parameter_detail::Read(body, source.body_offset + i * 4)});
  }
  parameter_detail::Commit(writes);
  return {};
}

// Each literal/masked block must fit wholly within one explicitly mapped byte
// region. Region declarations cannot overlap, though successive table writes
// may overlap and are applied in table order. External blocks are ignored here.
inline ParameterUpdateResult ApplyBindParameterPatches(
    std::span<const uint8_t> table,
    std::span<const ParameterRegion> regions) {
  const auto metadata = ParseMetadata(table);
  if (!metadata.valid()) return {metadata.error};
  for (size_t i = 0; i < regions.size(); ++i) {
    const auto& region = regions[i];
    if (region.byte_offset % 4 != 0) return {"unaligned parameter region"};
    if (region.words.size() > (uint64_t{0x100000000} - region.byte_offset) / 4)
      return {"parameter region exceeds byte address space"};
    for (size_t j = 0; j < i; ++j) {
      const auto& other = regions[j];
      if (!region.words.empty() && !other.words.empty() &&
          uint64_t{region.byte_offset} < uint64_t{other.byte_offset} + other.words.size_bytes() &&
          uint64_t{other.byte_offset} < uint64_t{region.byte_offset} + region.words.size_bytes())
        return {"overlapping parameter regions"};
      if (parameter_detail::Overlap(region.words, other.words))
        return {"overlapping host parameter destinations"};
    }
  }
  std::vector<parameter_detail::Write> writes;
  for (const auto& block : metadata.blocks) {
    if (block.group == Group::External) continue;
    if (block.offset % 4 != 0) return {"unaligned parameter destination"};
    const size_t count = block.group == Group::Literal ? block.count : block.count / 2;
    std::span<uint32_t> target;
    for (const auto& region : regions) {
      if (block.offset < region.byte_offset) continue;
      const size_t index = (block.offset - region.byte_offset) / 4;
      if (index <= region.words.size() && count <= region.words.size() - index) {
        target = region.words.subspan(index, count);
        break;
      }
    }
    if (target.empty()) return {"parameter destination outside mapped regions"};
    for (size_t i = 0; i < count; ++i) {
      const size_t at = block.payload_offset + i * (block.group == Group::Literal ? 4 : 8);
      const uint32_t first = parameter_detail::Read(table, at);
      writes.push_back({&target[i], block.group == Group::Literal ? 0 : first,
          block.group == Group::Literal ? first : parameter_detail::Read(table, at + 4)});
    }
  }
  parameter_detail::Commit(writes);
  return {};
}

}  // namespace copa::shader

