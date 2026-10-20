// Diagnostics only (COPA_DIAG_SHADOW=1): logs the inputs of the flat-shadow matrix builder
// sub_83024FF8 (LightWorldViewProjection, c32). The original always runs.
#include <cstdlib>
#include <cstring>
#include <string>

#include <fmt/format.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/thread_state.h>

#include "native/guest_device.h"

namespace {
std::string Matrix(uint8_t* base, uint32_t address) {
  if (!copa::ValidGuest(address)) return "-";
  std::string s;
  for (int i = 0; i < 16; ++i) {
    uint32_t v = copa::LoadBE32(copa::GuestPtr(base, address + i * 4));
    float f;
    std::memcpy(&f, &v, 4);
    s += fmt::format("{}{:g}", i ? (i % 4 ? "," : " | ") : "", f);
  }
  return s;
}
}  // namespace

REX_EXTERN(__imp__sub_83024FF8);
REX_HOOK_RAW(sub_83024FF8) {
  static const bool enabled = std::getenv("COPA_DIAG_SHADOW") != nullptr;
  static int calls = 0;
  if (enabled && (calls++ % 300) == 0) {
    uint32_t obj = copa::LoadBE32(copa::GuestPtr(base, ctx.r3.u32 + 8));
    REXLOG_INFO("diag 83024FF8 r3={:08X} r4={:08X} r5={:08X} r6={:08X} obj={:08X}", ctx.r3.u32, ctx.r4.u32,
                ctx.r5.u32, ctx.r6.u32, obj);
    REXLOG_INFO("diag A: {}", Matrix(base, ctx.r5.u32));
    REXLOG_INFO("diag r4: {}", Matrix(base, ctx.r4.u32));
    REXLOG_INFO("diag r6: {} | obj+30C: {}", Matrix(base, ctx.r6.u32).substr(0, 80), Matrix(base, obj + 0x300).substr(0, 80));
    for (uint32_t i = 0; i < 4; ++i)
      REXLOG_INFO("diag B{}: {} | extra {}", i, Matrix(base, obj + 0x1C0 + 0x50 * i),
                  Matrix(base, obj + 0x1C0 + 0x50 * i + 0x40).substr(0, 60));
  }
  __imp__sub_83024FF8(ctx, base);
}

// Diagnostics only (COPA_WATCH=<guest physical hex>): reports the host code that writes the
// 4 bytes at that physical address, through any guest view (0xA/0xC/0xE0000000) or the
// physical base, by keeping the page read-only and single-stepping each faulting write.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace {
struct WatchHit {
  uintptr_t rip;
  uintptr_t target;
  uint32_t guest_chain[8];  // guest LR, then return addresses from the PPC back chain
};
constexpr int kWatchViews = 4;
uintptr_t g_watch_pages[kWatchViews];
uintptr_t g_watch_target[kWatchViews];
std::atomic<int> g_watch_hits{0};
WatchHit g_watch_log[256];
thread_local uintptr_t t_watch_reprotect = 0;
bool g_watch_reads = false;  // COPA_WATCH=r<hex>: reads too
DWORD g_watch_protect = PAGE_READONLY;

bool WatchPage(uintptr_t address, uintptr_t* page) {
  for (int i = 0; i < kWatchViews; ++i)
    if (address >= g_watch_pages[i] && address < g_watch_pages[i] + 0x1000) {
      *page = g_watch_pages[i];
      return true;
    }
  return false;
}

LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* e) {
  DWORD code = e->ExceptionRecord->ExceptionCode;
  DWORD old;
  if (code == EXCEPTION_SINGLE_STEP && t_watch_reprotect) {
    VirtualProtect(reinterpret_cast<void*>(t_watch_reprotect), 0x1000, g_watch_protect, &old);
    t_watch_reprotect = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  uintptr_t page;
  if (code != EXCEPTION_ACCESS_VIOLATION ||
      (!g_watch_reads && e->ExceptionRecord->ExceptionInformation[0] != 1) ||
      !WatchPage(e->ExceptionRecord->ExceptionInformation[1], &page))
    return EXCEPTION_CONTINUE_SEARCH;
  uintptr_t target = e->ExceptionRecord->ExceptionInformation[1];
  bool hit = g_watch_reads;  // reads: any access to the page, one entry per code address
  for (int i = 0; i < kWatchViews; ++i)
    hit |= target >= g_watch_target[i] - 16 && target < g_watch_target[i] + 16;
  uintptr_t rip = uintptr_t(e->ContextRecord->Rip);
  if (g_watch_reads) {
    int known = std::min(g_watch_hits.load(), 256);
    for (int i = 0; i < known && hit; ++i) hit = g_watch_log[i].rip != rip;
  }
  if (hit) {
    int n = g_watch_hits.fetch_add(1);
    if (n < 256) {
      g_watch_log[n] = {rip, target, {}};
      if (auto* ts = rex::runtime::ThreadState::Get()) {
        auto* c = ts->context();
        auto be = [](uint32_t a) {
          uint32_t v;
          std::memcpy(&v, reinterpret_cast<const uint8_t*>(0x200000000ull + a), 4);
          return _byteswap_ulong(v);
        };
        g_watch_log[n].guest_chain[0] = uint32_t(c->lr);
        uint32_t sp = c->r1.u32;
        for (int k = 1; k < 8 && sp > 0x10000 && sp < 0xF0000000u; ++k) {
          uint32_t prev = be(sp);
          if (prev <= sp || prev - sp > 0x100000) break;
          g_watch_log[n].guest_chain[k] = be(prev - 8);
          sp = prev;
        }
      }
    }
  }
  VirtualProtect(reinterpret_cast<void*>(page), 0x1000, PAGE_READWRITE, &old);
  t_watch_reprotect = page;
  e->ContextRecord->EFlags |= 0x100;
  return EXCEPTION_CONTINUE_EXECUTION;
}

struct WatchStarter {
  WatchStarter() {
    const char* env = std::getenv("COPA_WATCH");
    if (!env) return;
    g_watch_reads = env[0] == 'r';
    if (g_watch_reads) g_watch_protect = PAGE_NOACCESS;
    // COPA_WATCH=v<hex>: a guest virtual address (the XEX image) instead of a physical one.
    const bool virt = env[0] == 'v';
    uint32_t physical = uint32_t(std::strtoul(env + (g_watch_reads || virt ? 1 : 0), nullptr, 16));
    uintptr_t targets[kWatchViews] = {0x200000000ull + 0xA0000000u + physical,
                                      0x200000000ull + 0xC0000000u + physical,
                                      0x200000000ull + 0xE0000000u + physical + 0x1000,
                                      0x300000000ull + physical};
    if (virt)
      for (auto& t : targets) t = 0x200000000ull + physical;
    for (int i = 0; i < kWatchViews; ++i) {
      g_watch_target[i] = targets[i];
      g_watch_pages[i] = targets[i] & ~uintptr_t(0xFFF);
    }
    AddVectoredExceptionHandler(1, WatchHandler);
    std::thread([virt] {
      // An image address is written while the XEX loads: watch once the game runs.
      if (virt) std::this_thread::sleep_for(std::chrono::seconds(8));
      int printed = 0;
      for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        for (int i = 0; i < kWatchViews; ++i) {
          MEMORY_BASIC_INFORMATION mbi;
          if (VirtualQuery(reinterpret_cast<void*>(g_watch_pages[i]), &mbi, sizeof(mbi)) &&
              mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE) {
            DWORD old;
            VirtualProtect(reinterpret_cast<void*>(g_watch_pages[i]), 0x1000, g_watch_protect, &old);
          }
        }
        int n = std::min(g_watch_hits.load(), 256);
        for (; printed < n; ++printed) {
          HMODULE module = nullptr;
          char name[MAX_PATH] = "?";
          GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCSTR>(g_watch_log[printed].rip), &module);
          if (module) GetModuleFileNameA(module, name, sizeof(name));
          std::string chain;
          for (uint32_t a : g_watch_log[printed].guest_chain)
            if (a) chain += fmt::format(" {:08X}", a);
          uint32_t now;
          std::memcpy(&now, reinterpret_cast<const void*>(g_watch_log[printed].target & ~uintptr_t(3)), 4);
          REXLOG_INFO("copa watch: access {:X} by rip={:X} ({}+{:X}) now={:08X} guest:{}", g_watch_log[printed].target,
                      g_watch_log[printed].rip, name, g_watch_log[printed].rip - uintptr_t(module),
                      _byteswap_ulong(now), chain);
        }
      }
    }).detach();
  }
} g_watch_starter;

// Diagnostics only (COPA_PEEK=<guest virtual hex>): logs every change of the 16 bytes there,
// polled every millisecond (no page protection, so it cannot disturb the game).
struct PeekStarter {
  PeekStarter() {
    const char* env = std::getenv("COPA_PEEK");
    if (!env) return;
    uint32_t address = uint32_t(std::strtoul(env, nullptr, 16));
    std::thread([address] {
      auto start = std::chrono::steady_clock::now();
      const uint8_t* p = reinterpret_cast<const uint8_t*>(0x200000000ull + address);
      uint32_t last[4] = {1, 1, 1, 1};
      for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) continue;
        uint32_t now[4];
        std::memcpy(now, p, 16);
        if (std::memcmp(now, last, 16)) {
          std::memcpy(last, now, 16);
          REXLOG_INFO("copa peek {:08X} at {} ms: {:08X} {:08X} {:08X} {:08X}", address,
                      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count(),
                      _byteswap_ulong(now[0]), _byteswap_ulong(now[1]), _byteswap_ulong(now[2]), _byteswap_ulong(now[3]));
        }
      }
    }).detach();
  }
} g_peek_starter;
}  // namespace

// Diagnostics only (COPA_DIAG_POOL=1): the boot crash is a destructor (sub_836DA438) deleting a
// garbage member: the object was already destroyed. Logs every destruction of that class and of
// its holder (sub_82387810), with the guest call chain. The originals always run.
namespace {
bool PoolDiag() {
  static const bool enabled = std::getenv("COPA_DIAG_POOL") != nullptr;
  return enabled;
}
std::string GuestChain(PPCContext& ctx, uint8_t* base) {
  std::string chain = fmt::format("{:08X}", uint32_t(ctx.lr));
  uint32_t sp = ctx.r1.u32;
  for (int i = 0; i < 8 && sp > 0x10000 && sp < 0xF0000000u; ++i) {
    uint32_t prev = copa::LoadBE32(copa::GuestPtr(base, sp));
    if (prev <= sp || prev - sp > 0x100000) break;
    chain += fmt::format(" {:08X}", copa::LoadBE32(copa::GuestPtr(base, prev - 8)));
    sp = prev;
  }
  return chain;
}
std::string Words(uint8_t* base, uint32_t a) {
  if (a < 0x10000u) return "-";
  return fmt::format("{:08X} {:08X} {:08X} {:08X}", copa::LoadBE32(copa::GuestPtr(base, a)),
                     copa::LoadBE32(copa::GuestPtr(base, a + 4)), copa::LoadBE32(copa::GuestPtr(base, a + 8)),
                     copa::LoadBE32(copa::GuestPtr(base, a + 12)));
}
std::atomic<int> g_pool_logs{0};
}  // namespace

// Write watch on the held pointer (+8) of every live object of that class: page-protected,
// writes to other addresses of the page are single-stepped through.
namespace {
std::mutex g_yw_mutex;
std::unordered_map<uint32_t, int> g_yw_addresses;  // guest address of a watched +8 field
std::unordered_map<uintptr_t, int> g_yw_pages;     // host page -> watched fields on it
thread_local uintptr_t t_yw_reprotect = 0;
std::atomic<int> g_yw_hits{0};
uintptr_t HostOf(uint32_t guest) { return 0x200000000ull + guest; }

LONG CALLBACK YWatchHandler(EXCEPTION_POINTERS* e) {
  DWORD code = e->ExceptionRecord->ExceptionCode;
  DWORD old;
  if (code == EXCEPTION_SINGLE_STEP && t_yw_reprotect) {
    std::lock_guard<std::mutex> lock(g_yw_mutex);
    if (g_yw_pages.count(t_yw_reprotect))
      VirtualProtect(reinterpret_cast<void*>(t_yw_reprotect), 0x1000, PAGE_READONLY, &old);
    t_yw_reprotect = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (code != EXCEPTION_ACCESS_VIOLATION || e->ExceptionRecord->ExceptionInformation[0] != 1)
    return EXCEPTION_CONTINUE_SEARCH;
  uintptr_t target = e->ExceptionRecord->ExceptionInformation[1];
  uintptr_t page = target & ~uintptr_t(0xFFF);
  bool hit = false;
  {
    std::lock_guard<std::mutex> lock(g_yw_mutex);
    if (!g_yw_pages.count(page)) return EXCEPTION_CONTINUE_SEARCH;
    if (target >= 0x200000000ull && target < 0x300000000ull) {
      uint32_t g = uint32_t(target - 0x200000000ull);
      for (uint32_t a = (g & ~3u); a <= (g & ~3u) + 4; a += 4) hit |= g_yw_addresses.count(a) != 0;  // (a 64-bit store may start 4 before)
    }
  }
  if (hit && g_yw_hits.fetch_add(1) < 300) {
    std::string chain;
    if (auto* ts = rex::runtime::ThreadState::Get()) {
      auto* c = ts->context();
      chain = fmt::format("lr={:08X} r3={:08X} r31={:08X} r30={:08X}", uint32_t(c->lr), c->r3.u32, c->r31.u32,
                          c->r30.u32);
      uint32_t sp = c->r1.u32;
      for (int k = 0; k < 10 && sp > 0x10000 && sp < 0xF0000000u; ++k) {
        uint32_t prev = _byteswap_ulong(*reinterpret_cast<const uint32_t*>(HostOf(sp)));
        if (prev <= sp || prev - sp > 0x100000) break;
        chain += fmt::format(" {:08X}", _byteswap_ulong(*reinterpret_cast<const uint32_t*>(HostOf(prev - 8))));
        sp = prev;
      }
    }
    HMODULE exe = GetModuleHandleW(nullptr);
    REXLOG_INFO("diag pool WRITE to watched +8 at {:08X} by rip=exe+{:X} guest {}", uint32_t(target - 0x200000000ull),
                uintptr_t(e->ContextRecord->Rip) - uintptr_t(exe), chain);
  }
  VirtualProtect(reinterpret_cast<void*>(page), 0x1000, PAGE_READWRITE, &old);
  t_yw_reprotect = page;
  e->ContextRecord->EFlags |= 0x100;
  return EXCEPTION_CONTINUE_EXECUTION;
}

void YWatchAdd(uint32_t field) {
  static std::once_flag once;
  std::call_once(once, [] { AddVectoredExceptionHandler(1, YWatchHandler); });
  std::lock_guard<std::mutex> lock(g_yw_mutex);
  ++g_yw_addresses[field];
  uintptr_t page = HostOf(field) & ~uintptr_t(0xFFF);
  DWORD old;
  if (g_yw_pages[page]++ == 0) VirtualProtect(reinterpret_cast<void*>(page), 0x1000, PAGE_READONLY, &old);
}
void YWatchRemove(uint32_t field) {
  std::lock_guard<std::mutex> lock(g_yw_mutex);
  auto it = g_yw_addresses.find(field);
  if (it == g_yw_addresses.end()) return;
  if (--it->second == 0) g_yw_addresses.erase(it);
  uintptr_t page = HostOf(field) & ~uintptr_t(0xFFF);
  DWORD old;
  if (--g_yw_pages[page] == 0) {
    g_yw_pages.erase(page);
    VirtualProtect(reinterpret_cast<void*>(page), 0x1000, PAGE_READWRITE, &old);
  }
}
}  // namespace

// COPA_HWBP=<guest hex>: hardware write breakpoint (DR0, 4 bytes) on that address in every
// thread, armed when an object of the class is constructed with its +8 field there.
namespace {
uint32_t HwbpAddress() {
  static const uint32_t a = std::getenv("COPA_HWBP") ? uint32_t(std::strtoul(std::getenv("COPA_HWBP"), nullptr, 16)) : 0;
  return a;
}
std::atomic<int> g_hwbp_hits{0};
LONG CALLBACK HwbpHandler(EXCEPTION_POINTERS* e) {
  if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(e->ContextRecord->Dr6 & 1))
    return EXCEPTION_CONTINUE_SEARCH;
  e->ContextRecord->Dr6 = 0;
  if (g_hwbp_hits.fetch_add(1) < 3000) {
    std::string chain;
    if (auto* ts = rex::runtime::ThreadState::Get()) {
      auto* c = ts->context();
      chain = fmt::format("lr={:08X} r1={:08X} r3={:08X} r4={:08X} r5={:08X} r30={:08X} r31={:08X}", uint32_t(c->lr),
                          c->r1.u32, c->r3.u32, c->r4.u32, c->r5.u32, c->r30.u32, c->r31.u32);
      uint32_t sp = c->r1.u32;
      for (int k = 0; k < 12 && sp > 0x10000 && sp < 0xF0000000u; ++k) {
        uint32_t prev = _byteswap_ulong(*reinterpret_cast<const uint32_t*>(HostOf(sp)));
        if (prev <= sp || prev - sp > 0x100000) break;
        chain += fmt::format(" {:08X}", _byteswap_ulong(*reinterpret_cast<const uint32_t*>(HostOf(prev - 8))));
        sp = prev;
      }
    } else {
      chain = "(host thread)";
    }
    HMODULE exe = GetModuleHandleW(nullptr);
    uint32_t now = _byteswap_ulong(*reinterpret_cast<const uint32_t*>(HostOf(HwbpAddress())));
    REXLOG_INFO("diag HWBP write {:08X} now={:08X} thread={} rip=exe+{:X} guest {}", HwbpAddress(), now,
                GetCurrentThreadId(), uintptr_t(e->ContextRecord->Rip) - uintptr_t(exe), chain);
  }
  return EXCEPTION_CONTINUE_EXECUTION;
}
void ArmHwbp() {
  static std::atomic<bool> armed{false};
  if (armed.exchange(true)) return;
  AddVectoredExceptionHandler(1, HwbpHandler);
  // Another thread sets the debug registers of every thread (including this one, suspended).
  DWORD self = GetCurrentThreadId();
  std::thread([self] {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{sizeof(te)};
    int count = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
      if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
      HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
      if (!t) continue;
      if (SuspendThread(t) != DWORD(-1)) {
        CONTEXT c{};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(t, &c)) {
          c.Dr0 = HostOf(HwbpAddress());
          c.Dr7 = (c.Dr7 & ~0xF0003ull) | 1 | (1ull << 16) | (3ull << 18);  // L0, write, 4 bytes
          if (SetThreadContext(t, &c)) ++count;
        }
        ResumeThread(t);
      }
      CloseHandle(t);
    }
    CloseHandle(snap);
    REXLOG_INFO("diag HWBP armed on {:08X} in {} threads (requested by {})", HwbpAddress(), count, self);
  }).join();
}
}  // namespace

REX_EXTERN(__imp__sub_82387810);
REX_HOOK_RAW(sub_82387810) {
  if (PoolDiag() && g_pool_logs.fetch_add(1) < 5000) {
    uint32_t held = copa::LoadBE32(copa::GuestPtr(base, ctx.r3.u32 + 8));
    REXLOG_INFO("diag pool holder dtor X={:08X} Y={:08X} Y=[{}] thread={:08X} chain: {}", ctx.r3.u32, held,
                Words(base, held), ctx.r13.u32, GuestChain(ctx, base));
  }
  __imp__sub_82387810(ctx, base);
}

REX_EXTERN(__imp__sub_836DA438);
REX_HOOK_RAW(sub_836DA438) {
  if (PoolDiag() && !HwbpAddress()) YWatchRemove(ctx.r3.u32 + 8);
  if (PoolDiag() && g_pool_logs.fetch_add(1) < 5000)
    REXLOG_INFO("diag pool Y dtor this={:08X} [{}] thread={:08X} chain: {}", ctx.r3.u32, Words(base, ctx.r3.u32),
                ctx.r13.u32, GuestChain(ctx, base));
  __imp__sub_836DA438(ctx, base);
}

REX_EXTERN(__imp__sub_836DA378);
REX_HOOK_RAW(sub_836DA378) {
  uint32_t self = ctx.r3.u32, held = ctx.r4.u32;
  std::string chain = PoolDiag() ? GuestChain(ctx, base) : std::string();
  __imp__sub_836DA378(ctx, base);
  if (PoolDiag() && !HwbpAddress()) YWatchAdd(self + 8);
  if (HwbpAddress() && self + 8 == HwbpAddress()) ArmHwbp();
  if (PoolDiag() && g_pool_logs.fetch_add(1) < 5000)
    REXLOG_INFO("diag pool Y ctor this={:08X} held={:08X} [{}] thread={:08X} chain: {}", self, held,
                Words(base, self), ctx.r13.u32, chain);
}
