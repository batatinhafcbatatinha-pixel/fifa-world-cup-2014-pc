// Copa 2014 native graphics system.
//
// Replaces the SDK GraphicsSystem (Xenos command processor, MMIO handlers, EDRAM)
// with OS-level services only:
//  - guest interrupt delivery: 60 Hz vblank plus "GPU callback" interrupts the game's
//    D3D library requests at the end of a frame (flip) or for fences;
//  - a plain RAM page where the Xenos register window used to be, so register stores
//    from the D3D library go nowhere and register loads return fixed values.
// Rendering itself is done by the D3D11 renderer in the game code (see d3d_hooks.cpp);
// nothing here parses or executes GPU command buffers.
//
// Header-only on purpose: the app creates the object, the recompiled game code
// reaches it through Runtime::instance()->graphics_system() and calls the inline methods.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>

#include <rex/chrono/clock.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

#include "../pc_settings.h"

namespace copa {

using rex::X_STATUS;

class NativeGraphicsSystem final : public rex::system::IGraphicsSystem {
 public:
  static constexpr uint32_t kMagic = 0x43504E47;  // 'CPNG'
  static constexpr uint32_t kRegisterWindow = 0x7FC80000;
  static constexpr uint32_t kRegisterWindowSize = 0x10000;

  // Returns the active native graphics system, or nullptr if the runtime uses another one.
  static NativeGraphicsSystem* Get() {
    auto* runtime = rex::Runtime::instance();
    if (!runtime) return nullptr;
    auto* graphics = static_cast<NativeGraphicsSystem*>(runtime->graphics_system());
    return graphics && graphics->magic_ == kMagic ? graphics : nullptr;
  }

  ~NativeGraphicsSystem() override { Shutdown(); }

  X_STATUS SetupPresentation(rex::ui::WindowedAppContext*) override { return X_STATUS_SUCCESS; }

  X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* dispatcher,
                         rex::system::KernelState* kernel_state) override {
    dispatcher_ = dispatcher;
    kernel_state_ = kernel_state;
    auto* memory = kernel_state->memory();
    // The window has no guest heap; commit plain host pages at its place in the arena
    // (the SDK commits the same range with no access for its MMIO handlers).
    if (!rex::memory::AllocFixed(memory->virtual_membase() + kRegisterWindow, 0x10000,
                                 rex::memory::AllocationType::kCommit,
                                 rex::memory::PageAccess::kReadWrite)) {
      REXLOG_ERROR("copa: could not commit RAM for the register window");
      return X_STATUS_UNSUCCESSFUL;
    }
    // Display controller status read by the interrupt handler (0x8244CF30):
    // bit 0 set means "this interrupt is a vblank".
    StoreRegister(0x6544, 1);
    running_ = true;
    thread_ = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
        kernel_state, 128 * 1024, 0, [this]() { return InterruptLoop(); }));
    thread_->set_name("Copa Interrupts");
    thread_->Create();
    return X_STATUS_SUCCESS;
  }

  bool has_presentation() const override { return false; }

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override {
    std::lock_guard lock(mutex_);
    callback_ = callback;
    callback_data_ = user_data;
  }

  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override {
    ring_base_ = ptr;
    ring_size_log2_ = size_log2;
  }

  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override {
    read_pointer_writeback_ = ptr;
    (void)block_size_log2;
  }

  void Shutdown() override {
    if (!running_.exchange(false)) return;
    wake_.notify_all();
    if (thread_) {
      thread_->Wait(0, 0, 0, nullptr);
      thread_.reset();
    }
  }

  // --- Called from the game code ------------------------------------------------

  // The D3D library asked for a CP interrupt that runs `function(argument)` once the GPU
  // reaches this point. Rendering is synchronous, so it is delivered right away from the
  // interrupt thread (never from inside the D3D call that requested it).
  void QueueGpuCallback(uint32_t status_block, uint32_t function, uint32_t argument) {
    {
      std::lock_guard lock(mutex_);
      callbacks_.push_back({status_block, function, argument});
    }
    wake_.notify_all();
  }

  // Ring read pointer as the D3D library expects the GPU to report it.
  void ReportReadPointer(uint32_t read_index) {
    if (read_pointer_writeback_ && kernel_state_) {
      auto* p = kernel_state_->memory()->TranslatePhysical<uint32_t*>(read_pointer_writeback_);
      *p = rex::byte_swap(read_index);
    }
  }

  // Called from the interrupt thread every millisecond (the game code publishes GPU
  // completion there once the D3D library's command-buffer workers are idle).
  void SetCompletionPoller(void (*poller)(void*), void* context) {
    poller_context_ = context;
    poller_.store(poller);
  }

  uint64_t vblank_count() const { return vblank_count_.load(); }
  double refresh_rate() const { return refresh_hz_; }
  void set_refresh_rate(double hz) { refresh_hz_ = hz > 1.0 ? hz : 60.0; }

 private:
  struct PendingCallback {
    uint32_t status_block, function, argument;
  };

  void StoreRegister(uint32_t offset, uint32_t value) {
    auto* p = reinterpret_cast<uint32_t*>(kernel_state_->memory()->virtual_membase() + kRegisterWindow + offset);
    *p = rex::byte_swap(value);
  }

  void Dispatch(uint32_t source) {
    uint32_t callback, data;
    {
      std::lock_guard lock(mutex_);
      callback = callback_;
      data = callback_data_;
    }
    if (!callback) return;
    auto* thread = rex::system::XThread::GetCurrentThread();
    thread->SetActiveCpu(2);
    uint64_t args[] = {source, data};
    dispatcher_->ExecuteInterrupt(thread->thread_state(), callback, args, 2);
  }

  int InterruptLoop() {
    using clock = std::chrono::steady_clock;
    auto next_vblank = clock::now();
    while (running_) {
      std::deque<PendingCallback> callbacks;
      {
        std::unique_lock lock(mutex_);
        auto deadline = std::min(next_vblank, clock::now() + std::chrono::milliseconds(1));
        wake_.wait_until(lock, deadline, [this] { return !callbacks_.empty() || !running_; });
        callbacks.swap(callbacks_);
      }
      for (const auto& cb : callbacks) {
        // What the CP did before raising the interrupt: publish function/argument in the
        // D3D status block (+0x10/+0x14), which handler 0x8244CF30 consumes for source 1.
        auto* memory = kernel_state_->memory();
        auto* block = memory->TranslateVirtual<uint32_t*>(cb.status_block);
        block[4] = rex::byte_swap(cb.function);
        block[5] = rex::byte_swap(cb.argument);
        Dispatch(1);
      }
      if (auto* poller = poller_.load()) poller(poller_context_);
      if (clock::now() >= next_vblank) {
        ++vblank_count_;
        Dispatch(0);
        next_vblank += std::chrono::nanoseconds(int64_t(1e9 / pcs::GuestRefreshHz()));  // live setting
        if (clock::now() > next_vblank + std::chrono::milliseconds(100)) next_vblank = clock::now();
      }
    }
    return 0;
  }

  uint32_t magic_ = kMagic;
  rex::runtime::FunctionDispatcher* dispatcher_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;
  rex::system::object_ref<rex::system::XHostThread> thread_;
  std::atomic<bool> running_{false};
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<PendingCallback> callbacks_;
  uint32_t callback_ = 0, callback_data_ = 0;
  uint32_t ring_base_ = 0, ring_size_log2_ = 0, read_pointer_writeback_ = 0;
  std::atomic<uint64_t> vblank_count_{0};
  std::atomic<void (*)(void*)> poller_{nullptr};
  void* poller_context_ = nullptr;
  double refresh_hz_ = pcs::GuestRefreshHz();
};

}  // namespace copa
