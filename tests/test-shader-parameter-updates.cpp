#include "../project/src/shader_parameter_updates.h"
#include <array>
#include <cassert>
#include <iostream>
using namespace copa::shader;
using Bytes = std::vector<uint8_t>;
static void Word(Bytes& b, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<uint8_t>(v >> s));
}
static Bytes Table(const Bytes& payload) {
  Bytes b(16); Word(b, static_cast<uint32_t>(payload.size()));
  b.insert(b.end(), payload.begin(), payload.end()); return b;
}
static ExternalConstantSource Source(uint32_t kind, uint32_t index, uint32_t count, uint32_t offset = 0) {
  return {0, 0, offset, kind, index, count};
}
int main() {
  std::array<uint32_t, 4> words{1,2,3,4};
  const std::array<ConstantDestination,1> destinations{{{7, words}}};
  Bytes body; Word(body, 0x11223344); Word(body, 0xaabbccdd);
  std::array<ExternalConstantSource,2> sources{Source(7,1,2), Source(7,2,1)};
  assert(ApplyExternalConstants(body, sources, destinations).valid());
  assert((words == std::array<uint32_t,4>{1,0x11223344,0x11223344,4}));
  const auto before = words;
  for (auto bad : {Source(8,0,1), Source(7,4,1), Source(7,0,3), Source(7,0,1,1), Source(7,0,1,0xffffffff)}) {
    sources[1] = bad;
    assert(!ApplyExternalConstants(body, sources, destinations).valid()); assert(words == before);
  }
  std::array<ConstantDestination,2> duplicate{{{7,words},{7,{}}}};
  assert(!ApplyExternalConstants(body, {}, duplicate).valid());
  duplicate[1] = {8,words};
  assert(!ApplyExternalConstants(body, {}, duplicate).valid());
  sources[0] = Source(7,4,0,8);
  assert(ApplyExternalConstants(body, std::span(sources).first(1), destinations).valid());
  // Aliased byte source is snapshotted before writes, including later entries.
  words = {0x01020304,0xa1b2c3d4,0,0};
  auto alias = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(words.data()), 8);
  const auto second = parameter_detail::Read(alias,4);
  sources = {Source(7,1,1,0),Source(7,2,1,4)};
  assert(ApplyExternalConstants(alias,sources,destinations).valid()); assert(words[2] == second);

  Bytes payload; Word(payload,0); // external sentinel
  Word(payload,(100u<<16)|2); Word(payload,0x12345678); Word(payload,0xabcdef01);
  Word(payload,0); // literal sentinel
  Word(payload,(100u<<16)|2); Word(payload,0xffff00ff); Word(payload,0x0000aa00);
  Word(payload,(100u<<16)|2); Word(payload,0xffffff0f); Word(payload,0x000000b0);
  const auto table = Table(payload);
  const std::array<ParameterRegion,1> regions{{{100,words}}};
  assert(ApplyBindParameterPatches(table,regions).valid());
  assert(words[0] == 0x1234aab8 && words[1] == 0xabcdef01);
  const auto patched = words;
  auto bad_payload = payload; Word(bad_payload,(116u<<16)|2); Word(bad_payload,0); Word(bad_payload,1);
  assert(!ApplyBindParameterPatches(Table(bad_payload),regions).valid()); assert(words == patched);
  bad_payload = payload; Word(bad_payload,(101u<<16)|2); Word(bad_payload,0); Word(bad_payload,1);
  assert(!ApplyBindParameterPatches(Table(bad_payload),regions).valid()); assert(words == patched);
  auto truncated = table; truncated.pop_back();
  assert(!ApplyBindParameterPatches(truncated,regions).valid()); assert(words == patched);
  std::array<uint32_t,4> other{};
  std::array<ParameterRegion,2> overlapping{{{100,words},{104,other}}};
  assert(!ApplyBindParameterPatches(table,overlapping).valid()); assert(words == patched);
  overlapping[1] = {200,words};
  assert(!ApplyBindParameterPatches(table,overlapping).valid());
  overlapping[1] = {201,other};
  assert(!ApplyBindParameterPatches(table,overlapping).valid());
  overlapping[1] = {0xfffffffc,other};
  assert(!ApplyBindParameterPatches(table,overlapping).valid());
  assert(!ApplyBindParameterPatches(table,{}).valid());
  std::cout << "shader parameter update tests passed\n";
}
