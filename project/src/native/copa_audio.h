// Copa 2014 native audio system (replaces the SDK AudioSystem and its XMA decoder).
//
// The game's audio engine talks to the XMA context array directly and requests work by
// writing context bit masks to a small register window (Kick / Lock / Clear) instead of
// always going through the kernel XMA functions. This class owns that window: it keeps
// the few registers the game reads (context array address, a rotating "current context")
// and forwards Kick / Lock / Clear synchronously to the native XMA engine in the game
// module (native_audio.cpp), which decodes with libavcodec. Output goes to XAudio2.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/audio.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace copa {

using rex::X_STATUS;

class NativeAudioSystem final : public rex::system::IAudioSystem {
 public:
  static constexpr uint32_t kMagic = 0x43504158;  // 'CPAX'
  static constexpr uint32_t kRegisterWindow = 0x7FEA0000;
  static constexpr uint32_t kContextCount = 320;
  static constexpr uint32_t kContextSize = 64;

  // Native XMA engine entry points, installed by the game code.
  struct Handlers {
    void (*kick)(uint32_t context_index) = nullptr;
    void (*lock)(uint32_t context_index) = nullptr;
    void (*clear)(uint32_t context_index) = nullptr;
  };

  static NativeAudioSystem* Get() {
    auto* runtime = rex::Runtime::instance();
    if (!runtime) return nullptr;
    auto* audio = static_cast<NativeAudioSystem*>(runtime->audio_system());
    return audio && audio->magic_ == kMagic ? audio : nullptr;
  }

  X_STATUS Setup(rex::system::KernelState* kernel_state) override {
    auto* memory = kernel_state->memory();
    contexts_virtual_ = memory->SystemHeapAlloc(kContextCount * kContextSize, 256,
                                                rex::memory::kSystemHeapPhysical);
    if (!contexts_virtual_) return X_STATUS_NO_MEMORY;
    std::memset(memory->TranslateVirtual(contexts_virtual_), 0, kContextCount * kContextSize);
    contexts_physical_ = memory->GetPhysicalAddress(contexts_virtual_);
    registers_.fill(0);
    registers_[0x600] = contexts_physical_;  // ContextArrayAddress
    if (!memory->AddVirtualMappedRange(kRegisterWindow, 0xFFFF0000, 0xFFFF, this, ReadThunk,
                                       WriteThunk)) {
      REXLOG_ERROR("copa audio: could not map the XMA register window");
      return X_STATUS_UNSUCCESSFUL;
    }
    REXLOG_INFO("copa audio: XMA context array at {:08X} (physical {:08X})", contexts_virtual_,
                contexts_physical_);
    return X_STATUS_SUCCESS;
  }

  void Shutdown() override {}

  void SetHandlers(const Handlers& handlers) {
    kick_.store(handlers.kick);
    lock_.store(handlers.lock);
    clear_.store(handlers.clear);
  }
  uint32_t contexts_virtual() const { return contexts_virtual_; }
  uint32_t contexts_physical() const { return contexts_physical_; }

 private:
  static uint32_t ReadThunk(void*, void* self, uint32_t addr) {
    return static_cast<NativeAudioSystem*>(self)->Read(addr);
  }
  static void WriteThunk(void*, void* self, uint32_t addr, uint32_t value) {
    static_cast<NativeAudioSystem*>(self)->Write(addr, value);
  }

  uint32_t Read(uint32_t addr) {
    uint32_t r = (addr & 0xFFFF) / 4;
    if (r >= registers_.size()) return 0;
    if (r == 0x606) {
      // CurrentContextIndex: rotate so the game's lock code never sees a stuck context.
      registers_[0x606] = next_context_;
      next_context_ = (next_context_ + 1) % kContextCount;
    }
    return rex::byte_swap(registers_[r]);
  }

  void Write(uint32_t addr, uint32_t value) {
    uint32_t r = (addr & 0xFFFF) / 4;
    value = rex::byte_swap(value);
    if (r >= registers_.size()) return;
    registers_[r] = value;
    auto dispatch = [&](uint32_t first, std::atomic<void (*)(uint32_t)>& handler) {
      auto* fn = handler.load();
      uint32_t base_index = (r - first) * 32;
      for (uint32_t bit = 0; bit < 32 && value; ++bit, value >>= 1)
        if ((value & 1) && fn && base_index + bit < kContextCount) fn(base_index + bit);
    };
    if (r >= 0x650 && r < 0x65A)
      dispatch(0x650, kick_);
    else if (r >= 0x690 && r < 0x69A)
      dispatch(0x690, lock_);
    else if (r >= 0x6A0 && r < 0x6AA)
      dispatch(0x6A0, clear_);
  }

  uint32_t magic_ = kMagic;
  uint32_t contexts_virtual_ = 0, contexts_physical_ = 0;
  std::array<uint32_t, 0x4000> registers_{};
  uint32_t next_context_ = 0;
  std::atomic<void (*)(uint32_t)> kick_{nullptr}, lock_{nullptr}, clear_{nullptr};
};

}  // namespace copa
