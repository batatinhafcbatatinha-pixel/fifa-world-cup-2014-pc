// Frame counter on the game's D3D present: sub_82453DC0 is the only caller of VdSwap.
// Logs "[fps]" once per second when COPA_FPS_LOG=1.

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <array>
#include <mutex>
#include <unordered_set>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <rex/hook.h>
#include <rex/logging.h>

#include "generated/default/copa2014_init.h"

REX_EXTERN(__imp__sub_82453DC0);

REX_HOOK_RAW(sub_82453DC0) {
  static const bool enabled = [] {
    const char* v = std::getenv("COPA_FPS_LOG");
    return v && *v == '1';
  }();
  if (enabled) {
    using clock = std::chrono::steady_clock;
    static clock::time_point window_start = clock::now();
    static clock::time_point last = window_start;
    static uint32_t frames = 0;
    static double worst_ms = 0;
    auto now = clock::now();
    double dt = std::chrono::duration<double, std::milli>(now - last).count();
    last = now;
    if (frames && dt > worst_ms) worst_ms = dt;
    ++frames;
    double window = std::chrono::duration<double>(now - window_start).count();
    if (window >= 1.0) {
      REXLOG_INFO("[fps] {:.1f} fps, worst frame {:.1f} ms", frames / window, worst_ms);
      window_start = now;
      frames = 0;
      worst_ms = 0;
    }
  }
  __imp__sub_82453DC0(ctx, base);
}

// Read-only tracing of candidate EA/RNA presentation boundaries recovered from
// the shipped image. Names are inferred; addresses are version specific.
// Enable COPA_RENDER_TRACE=1. Keep the original body for every call.
namespace {
bool CopaRenderTraceEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("COPA_RENDER_TRACE");
    return value && *value == '1';
  }();
  return enabled;
}
void CopaTraceBoundary(const char* boundary, PPCContext& ctx,
                       std::atomic<uint64_t>& counter) {
  if (!CopaRenderTraceEnabled()) return;
  const uint64_t call = counter.fetch_add(1, std::memory_order_relaxed) + 1;
  if (call <= 8 || (call % 600) == 0) {
    REXLOG_INFO("[native-render] {} call={} r3={:08X} r4={:08X} r5={:08X} r6={:08X} r7={:08X} lr={:08X}",
                boundary, call, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                ctx.r6.u32, ctx.r7.u32, static_cast<uint32_t>(ctx.lr));
  }
}
}
REX_EXTERN(__imp__sub_836B17A0);
REX_HOOK_RAW(sub_836B17A0) {
  static std::atomic<uint64_t> calls{0};
  CopaTraceBoundary("EA.DeviceC.present-candidate@836B17A0", ctx, calls);
  __imp__sub_836B17A0(ctx, base);
}
REX_EXTERN(__imp__sub_82467568);
REX_HOOK_RAW(sub_82467568) {
  static std::atomic<uint64_t> calls{0};
  CopaTraceBoundary("resolve-candidate@82467568", ctx, calls);
  __imp__sub_82467568(ctx, base);
}
REX_EXTERN(__imp__sub_82468830);
REX_HOOK_RAW(sub_82468830) {
  static std::atomic<uint64_t> calls{0};
  CopaTraceBoundary("clear-candidate@82468830", ctx, calls);
  __imp__sub_82468830(ctx, base);
}

// The null-resource branch at 8244CA78 clears the descriptor type bits.
// 82458A10 initializes 26 texture slots (24 bytes each), followed by the
// vertex-fetch region. 8244C970 takes device/stage/resource in r3/r4/r5.
// Snapshots are read-only and non-atomic; concurrent writers remain possible.
namespace {
struct CopaTextureSnapshot {
  std::array<uint32_t, 6> words{};
  uint32_t binding = 0;
  bool readable = false;
};

CopaTextureSnapshot CopaReadTextureSnapshot(uint8_t* base, uint32_t device,
                                            uint32_t stage) {
  CopaTextureSnapshot snapshot;
  if (!device || stage >= 26 || uint64_t(device) + 12920 + 4 * stage + 4 > 0x100000000ull)
    return snapshot;
#ifdef _WIN32
  // REX_LOAD_U32 is an unchecked volatile dereference. Use a bounded OS read
  // so optional diagnostics cannot fault on an unreadable guest range. Keep
  // the generated REX_RAW_ADDR mapping (physical aliases have a host offset).
  const uint32_t slot = device + 1152 + 24 * stage;
  const uint32_t binding_address = device + 12920 + 4 * stage;
  SIZE_T read = 0;
  if (!ReadProcessMemory(GetCurrentProcess(), REX_RAW_ADDR(slot), snapshot.words.data(),
                         sizeof(snapshot.words), &read) || read != sizeof(snapshot.words))
    return snapshot;
  if (!ReadProcessMemory(GetCurrentProcess(), REX_RAW_ADDR(binding_address), &snapshot.binding,
                         sizeof(snapshot.binding), &read) || read != sizeof(snapshot.binding))
    return snapshot;
  for (uint32_t& word : snapshot.words) word = __builtin_bswap32(word);
  snapshot.binding = __builtin_bswap32(snapshot.binding);
  snapshot.readable = true;
#endif
  return snapshot;
}

void CopaLogTextureSnapshot(const char* phase, const CopaTextureSnapshot& s) {
  REXLOG_INFO("[native-render-texture] {} readable={} binding={:08X} type={} fetch=({:08X} {:08X} {:08X} {:08X} {:08X} {:08X})",
              phase, s.readable, s.binding, s.words[0] & 3,
              s.words[0], s.words[1], s.words[2], s.words[3], s.words[4], s.words[5]);
}
}
REX_EXTERN(__imp__sub_8244C970);
REX_HOOK_RAW(sub_8244C970) {
  if (!CopaRenderTraceEnabled()) {
    __imp__sub_8244C970(ctx, base);
    return;
  }
  static std::atomic<uint64_t> binds{0}, unbinds{0}, anomalies{0}, live_unbinds{0};
  const uint32_t device = ctx.r3.u32, stage = ctx.r4.u32, resource = ctx.r5.u32;
  const uint32_t caller = static_cast<uint32_t>(ctx.lr);
  const bool unbind = resource == 0;
  const uint64_t call = (unbind ? unbinds : binds).fetch_add(1, std::memory_order_relaxed) + 1;
  // Initial unbinds only clear empty slots. Keep a separate quota for real
  // texture-to-null transitions, or startup consumes all useful evidence.
  // Bound read overhead: after 4096 calls inspect one in 600, until the quota.
  // With tracing off the early return above performs no reads or counting.
  const bool needs_sample = unbind
      ? live_unbinds.load(std::memory_order_relaxed) < 16
      : anomalies.load(std::memory_order_relaxed) < 16;
  const bool inspect = stage < 26 && (call <= 16 ||
      (needs_sample && (call <= 4096 || (call % 600) == 0)));
  const CopaTextureSnapshot before = inspect ? CopaReadTextureSnapshot(base, device, stage)
                                             : CopaTextureSnapshot{};
  __imp__sub_8244C970(ctx, base);
  const CopaTextureSnapshot after = inspect ? CopaReadTextureSnapshot(base, device, stage)
                                            : CopaTextureSnapshot{};
  const bool anomalous_bind = !unbind && after.readable && (after.words[0] & 3) != 2;
  const uint64_t anomaly = anomalous_bind
      ? anomalies.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
  const bool live_unbind = unbind && before.readable && after.readable &&
      before.binding != 0 && (before.words[0] & 3) == 2 &&
      after.binding == 0 && (after.words[0] & 3) == 0;
  const uint64_t transition = live_unbind
      ? live_unbinds.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
  if (call <= 16 || (anomaly && anomaly <= 16) || (transition && transition <= 16)) {
    REXLOG_INFO("[native-render-texture] {} call={} anomaly={} device={:08X} stage={} valid-stage={} resource={:08X} lr={:08X}",
                unbind ? "unbind@8244C970" : "bind@8244C970", call, anomaly,
                device, stage, stage < 26, resource, caller);
    CopaLogTextureSnapshot("before", before);
    CopaLogTextureSnapshot("after", after);
  }
}

REX_EXTERN(__imp__sub_8245CD80);
REX_HOOK_RAW(sub_8245CD80) {
  static std::atomic<uint64_t> calls{0};
  CopaTraceBoundary("draw-vertices-candidate@8245CD80", ctx, calls);
  __imp__sub_8245CD80(ctx, base);
}
REX_EXTERN(__imp__sub_8245D198);
REX_HOOK_RAW(sub_8245D198) {
  static std::atomic<uint64_t> calls{0};
  CopaTraceBoundary("draw-indexed-candidate@8245D198", ctx, calls);
  __imp__sub_8245D198(ctx, base);
}
REX_EXTERN(__imp__sub_8245C758);
REX_HOOK_RAW(sub_8245C758) {
  static std::atomic<uint64_t> calls{0};
  CopaTraceBoundary("draw-vertices-up-candidate@8245C758", ctx, calls);
  __imp__sub_8245C758(ctx, base);
}

// Candidate shader/declaration bindings. Static stores target device+13068,
// +13072 and +12240 respectively; preserve lifetime, dirty flags and all other
// original side effects. These hooks observe bindings, not host shader objects.
namespace {
struct CopaFetchBank {
  std::array<std::array<uint32_t, 6>, 26> fetch{};
  std::array<uint32_t, 26> bindings{};
  bool readable = false;
};
CopaFetchBank CopaReadFetchBank(uint8_t* base, uint32_t device) {
  CopaFetchBank bank;
  if (!device || uint64_t(device) + 12920 + sizeof(bank.bindings) > 0x100000000ull)
    return bank;
#ifdef _WIN32
  SIZE_T read = 0;
  if (!ReadProcessMemory(GetCurrentProcess(), REX_RAW_ADDR(device + 1152),
                         bank.fetch.data(), sizeof(bank.fetch), &read) ||
      read != sizeof(bank.fetch)) return bank;
  if (!ReadProcessMemory(GetCurrentProcess(), REX_RAW_ADDR(device + 12920),
                         bank.bindings.data(), sizeof(bank.bindings), &read) ||
      read != sizeof(bank.bindings)) return bank;
  for (auto& fetch : bank.fetch)
    for (auto& word : fetch) word = __builtin_bswap32(word);
  for (auto& binding : bank.bindings) binding = __builtin_bswap32(binding);
  bank.readable = true;
#endif
  return bank;
}
bool CopaSampleShaderFetch(bool vertex, uint32_t device, uint32_t shader) {
  static const bool enabled = [] {
    const char* value = std::getenv("COPA_SHADER_FETCH_TRACE");
    return value && *value == '1';
  }();
  if (!enabled || !shader) return false;
  // A first-observation sample, not coverage of every bind. Keep diagnostics
  // finite even across a match. No lock spans the original guest function.
  static std::mutex mutex;
  static std::array<std::unordered_set<uint64_t>, 2> seen;
  static std::array<std::atomic<bool>, 2> full{};
  const size_t index = vertex ? 1 : 0;
  if (full[index].load(std::memory_order_relaxed)) return false;
  std::lock_guard lock(mutex);
  if (seen[index].size() >= 128) {
    full[index].store(true, std::memory_order_relaxed);
    return false;
  }
  return seen[index].insert((uint64_t(device) << 32) | shader).second;
}
template <typename Original>
void CopaTraceShaderFetch(bool vertex, Original original, PPCContext& ctx, uint8_t* base) {
  const uint32_t device = ctx.r3.u32, shader = ctx.r4.u32;
  if (!CopaSampleShaderFetch(vertex, device, shader)) {
    original(ctx, base);
    return;
  }
  const uint32_t caller = static_cast<uint32_t>(ctx.lr);
  const auto before = CopaReadFetchBank(base, device);
  original(ctx, base);
  const auto after = CopaReadFetchBank(base, device);
  unsigned changed = 0, cleared = 0;
  const char* kind = vertex ? "VS" : "PS";
  if (before.readable && after.readable) {
    for (size_t slot = 0; slot < before.fetch.size(); ++slot) {
      const auto& a = before.fetch[slot];
      const auto& b = after.fetch[slot];
      if (a == b && before.bindings[slot] == after.bindings[slot]) continue;
      ++changed;
      if ((a[0] & 3) == 2 && (b[0] & 3) == 0) ++cleared;
      REXLOG_INFO("[shader-fetch] kind={} device={:08X} shader={:08X} slot={} binding={:08X}->{:08X} before=({:08X} {:08X} {:08X} {:08X} {:08X} {:08X}) after=({:08X} {:08X} {:08X} {:08X} {:08X} {:08X})",
                  kind, device, shader, slot, before.bindings[slot], after.bindings[slot],
                  a[0], a[1], a[2], a[3], a[4], a[5], b[0], b[1], b[2], b[3], b[4], b[5]);
    }
  }
  REXLOG_INFO("[shader-fetch-summary] kind={} device={:08X} shader={:08X} lr={:08X} readable-before={} readable-after={} changed={} cleared={}",
              kind, device, shader, caller, before.readable, after.readable, changed, cleared);
}
}
REX_EXTERN(__imp__sub_8245B6E8);
REX_HOOK_RAW(sub_8245B6E8) {
  static std::atomic<uint64_t> calls{0};
  CopaTraceBoundary("bind-pixel-shader-candidate@8245B6E8", ctx, calls);
  CopaTraceShaderFetch(false, __imp__sub_8245B6E8, ctx, base);
}
REX_EXTERN(__imp__sub_8245B8F0);
REX_HOOK_RAW(sub_8245B8F0) {
  static std::atomic<uint64_t> calls{0};
  CopaTraceBoundary("bind-vertex-shader-candidate@8245B8F0", ctx, calls);
  CopaTraceShaderFetch(true, __imp__sub_8245B8F0, ctx, base);
}
REX_EXTERN(__imp__sub_8245BB08);
REX_HOOK_RAW(sub_8245BB08) {
  static std::atomic<uint64_t> calls{0};
  CopaTraceBoundary("bind-vertex-declaration-candidate@8245BB08", ctx, calls);
  __imp__sub_8245BB08(ctx, base);
}
