// Copa 2014 native audio, compiled with the recompiled game code.
//
// - XAudio render driver exports: the game's mixer callbacks run on an audio thread and
//   their 5.1 float frames go to XAudio2 (downmixed to stereo).
// - XMA exports: contexts live in guest memory exactly as the game expects; an XMA
//   worker decodes enabled contexts with libavcodec's XMA decoder (native code) and writes
//   16-bit big-endian PCM into the game's output ring buffers.
//
// The XMA context processing follows Xenia's xma_context.cc (BSD, Copyright Ben Vanik;
// "most of this code goes to libertyv/xma2dec.c"), adapted here without the APU register
// file: the kernel XMA functions act on the contexts directly.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifdef _WIN32
#include <windows.h>
#ifndef _WIN32
#include "../platform/native_win32_posix.h"
#endif
#include <xaudio2.h>
#else
#include <SDL3/SDL.h>
#endif
#include <bit>

#include <future>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

extern "C" {
#include "libavcodec/avcodec.h"
}

#include <rex/audio/xma/context.h>
#include <rex/audio/xma/helpers.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include "pc_settings.h"
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

#include "guest_device.h"
#include "copa_audio.h"

using rex::audio::XMA_CONTEXT_DATA;
namespace xma = rex::audio::xma;

namespace {

constexpr uint32_t kBytesPerPacket = 2048;
constexpr uint32_t kBitsPerPacket = kBytesPerPacket * 8;
constexpr uint32_t kBitsPerPacketHeader = 32;
constexpr uint32_t kBitsPerFrameHeader = 15;
constexpr uint32_t kBytesPerPacketHeader = 4;
constexpr uint32_t kBytesPerPacketData = kBytesPerPacket - kBytesPerPacketHeader;
constexpr uint32_t kBytesPerSample = 2;
constexpr uint32_t kSamplesPerFrame = 512;
constexpr uint32_t kBytesPerFrameChannel = kSamplesPerFrame * kBytesPerSample;
constexpr uint32_t kOutputBytesPerBlock = 256;
constexpr uint32_t kMaxFrameSizeInBits = 0x4000 - kBitsPerPacketHeader;
constexpr uint32_t kDecoderStartPadding = 192;
constexpr int kSampleRates[4] = {24000, 32000, 44100, 48000};

rex::memory::Memory* Memory() { return rex::Runtime::instance()->memory(); }

// Diagnostics counters (logged by the audio thread every 2 s).
std::atomic<uint64_t> g_xma_created{0}, g_xma_kicks{0}, g_xma_frames{0}, g_xma_errors{0},
    g_driver_callbacks{0}, g_driver_submits{0};
uint32_t LoadBE(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, copa::GuestPtr(const_cast<uint8_t*>(base), address), 4);
  return std::byteswap(v);
}
void StoreBE(uint8_t* base, uint32_t address, uint32_t value) {
  value = std::byteswap(value);
  std::memcpy(copa::GuestPtr(base, address), &value, 4);
}
uint8_t* Physical(uint32_t address) { return Memory()->TranslatePhysical(address); }

// ------------------------------------------------------------------------ bit stream
class BitStream {
 public:
  BitStream(uint8_t* buffer, size_t size_bits) : buffer_(buffer), size_(size_bits) {}
  void SetOffset(size_t o) { offset_ = std::min(o, size_); }
  size_t offset() const { return offset_; }
  size_t BitsRemaining() const { return size_ - offset_; }
  uint64_t Peek(size_t n) const {
    size_t byte = offset_ >> 3, rel = offset_ & 7;
    uint64_t bits;
    std::memcpy(&bits, buffer_ + byte, 8);
    bits = std::byteswap(bits);
    bits >>= 64 - (rel + n);
    return bits & ((1ull << n) - 1);
  }
  uint64_t Read(size_t n) {
    uint64_t v = Peek(n);
    SetOffset(offset_ + n);
    return v;
  }
  void Advance(size_t n) { SetOffset(offset_ + n); }
  size_t Copy(uint8_t* dest, size_t n) {
    size_t byte = offset_ >> 3, rel = offset_ & 7, left = n, out = 0;
    if (rel) {
      uint64_t bits = Peek(8 - rel);
      uint8_t clear = uint8_t(~((1u << rel) - 1));
      dest[out] = uint8_t((dest[out] & clear) | uint8_t(bits));
      left -= 8 - rel;
      Advance(8 - rel);
      ++out;
    }
    if (left >= 8) {
      std::memcpy(dest + out, buffer_ + byte + out, left / 8);
      out += left / 8;
      Advance((left / 8) * 8);
      left -= (left / 8) * 8;
    }
    if (left) {
      uint64_t bits = Peek(left) << (8 - left);
      uint8_t clear = uint8_t((1u << left) - 1);
      dest[out] = uint8_t((dest[out] & clear) | uint8_t(bits));
      Advance(left);
    }
    return rel;
  }

 private:
  uint8_t* buffer_;
  size_t size_, offset_ = 0;
};

// ------------------------------------------------------------------------ XMA context
struct PacketInfo {
  uint8_t frame_count = 0, current_frame = 0;
  uint32_t current_frame_size = 0;
  bool IsLastFrameInPacket() const { return frame_count == 0 || current_frame == frame_count - 1; }
};

class XmaContext {
 public:
  uint32_t guest = 0;  // virtual address of the 64-byte context
  std::atomic<bool> allocated{false}, enabled{false};
  std::mutex lock;

  bool Setup() {
    codec_ = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
    if (!codec_) return false;
    packet_ = av_packet_alloc();
    frame_av_ = av_frame_alloc();
    return packet_ && frame_av_;
  }

  void Work() {
    if (!allocated || !enabled) return;
    std::lock_guard g(lock);
    // Re-check under the lock: a Lock request between the check above and here disabled
    // the context so the game can rewrite it (new buffers / offsets); decoding it now would
    // race those edits and store a stale read offset over them.
    if (!enabled.exchange(false)) return;
    uint8_t* ptr = Memory()->TranslateVirtual(guest);
    XMA_CONTEXT_DATA data(ptr);
    const XMA_CONTEXT_DATA initial = data;
    if (!data.output_buffer_valid) return;
    PrepareOutput(&data);
    if (data.IsConsumeOnlyContext()) {
      if (remaining_subframes_ == 0) return;
      Consume(&data);
      data.output_buffer_write_offset = out_write_ / kOutputBytesPerBlock;
      StoreMerged(data, initial, ptr);
      return;
    }
    const int32_t minimum = int32_t(std::max<uint32_t>(1, data.subframe_decode_count)) +
                            int32_t(data.output_buffer_padding);
    if (minimum > free_blocks_) {
      StoreMerged(data, initial, ptr);
      return;
    }
    // Malformed input (every frame failing, e.g. inside a loop region) must neither spin
    // here holding the lock - the game's Lock request would block its audio thread - nor
    // play forever: after a long run of failed frames the context goes to the error state.
    for (int idle = 0; free_blocks_ >= minimum && idle < 16;) {
      const int32_t free_before = free_blocks_;
      const uint8_t remaining_before = remaining_subframes_;
      Decode(&data);
      Consume(&data);
      if (!data.IsAnyInputBufferValid() || data.error_status == 4) break;
      if (failed_frames_ > 512) {
        data.error_status = 4;
        break;
      }
      idle = (free_blocks_ == free_before && remaining_subframes_ == remaining_before) ? idle + 1 : 0;
    }
    data.output_buffer_write_offset = out_write_ / kOutputBytesPerBlock;
    if (out_write_ == out_read_ && out_used_ == 0) data.output_buffer_valid = 0;
    StoreMerged(data, initial, ptr);
  }

  void Clear() {
    std::lock_guard g(lock);
    enabled = false;
    uint8_t* ptr = Memory()->TranslateVirtual(guest);
    XMA_CONTEXT_DATA data(ptr);
    data.input_buffer_0_valid = 0;
    data.input_buffer_1_valid = 0;
    data.output_buffer_valid = 0;
    data.input_buffer_read_offset = kBitsPerPacketHeader;
    data.output_buffer_read_offset = 0;
    data.output_buffer_write_offset = 0;
    ResetDecoder();
    data.Store(ptr);
  }

  void Release() {
    std::lock_guard g(lock);
    allocated = false;
    enabled = false;
    ResetDecoder();
    std::memset(Memory()->TranslateVirtual(guest), 0, 64);
  }

  void ResetDecoder() {
    if (av_) avcodec_free_context(&av_);
    raw_.fill(0);
    decoded_.fill(0);
    carry_.fill(0);
    carry_valid_ = false;
    failed_frames_ = 0;
    pending_limit_ = pending_skip_ = 0;
    remaining_subframes_ = 0;
    loop_limit_ = 0;
    loop_skip_pending_ = false;
  }

 private:
  // Output ring (in 256-byte blocks).
  void PrepareOutput(XMA_CONTEXT_DATA* d) {
    out_base_ = Physical(d->output_buffer_ptr);
    out_capacity_ = d->output_buffer_block_count * kOutputBytesPerBlock;
    out_read_ = d->output_buffer_read_offset * kOutputBytesPerBlock;
    out_write_ = d->output_buffer_write_offset * kOutputBytesPerBlock;
    // Write space; read == write means empty (the game consumes before the decoder wraps).
    uint32_t used = out_write_ >= out_read_ ? out_write_ - out_read_
                                            : out_capacity_ - out_read_ + out_write_;
    out_used_ = used;
    free_blocks_ = int32_t(out_capacity_ - used) / int32_t(kOutputBytesPerBlock);
  }
  void Write(const uint8_t* src, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes && out_capacity_; ++i) {
      out_base_[out_write_] = src[i];
      out_write_ = (out_write_ + 1) % out_capacity_;
    }
    out_used_ += bytes;
  }

  void StoreMerged(const XMA_CONTEXT_DATA& data, const XMA_CONTEXT_DATA& initial, uint8_t* ptr) {
    XMA_CONTEXT_DATA fresh(ptr);
    fresh.loop_count = data.loop_count;
    fresh.output_buffer_write_offset = data.output_buffer_write_offset;
    if (initial.input_buffer_0_valid && !data.input_buffer_0_valid) fresh.input_buffer_0_valid = 0;
    if (initial.input_buffer_1_valid && !data.input_buffer_1_valid) fresh.input_buffer_1_valid = 0;
    if (initial.output_buffer_valid && !data.output_buffer_valid) fresh.output_buffer_valid = 0;
    fresh.input_buffer_read_offset = data.input_buffer_read_offset;
    fresh.error_status = data.error_status;
    fresh.current_buffer = data.current_buffer;
    fresh.output_buffer_read_offset = data.output_buffer_read_offset;
    fresh.Store(ptr);
  }

  static void SwapInput(XMA_CONTEXT_DATA* d) {
    if (d->current_buffer == 0) d->input_buffer_0_valid = 0; else d->input_buffer_1_valid = 0;
    d->current_buffer ^= 1;
    d->input_buffer_read_offset = kBitsPerPacketHeader;
  }

  void UpdateLoop(XMA_CONTEXT_DATA* d) {
    if (d->loop_count == 0) return;
    uint32_t start = std::max(kBitsPerPacketHeader, uint32_t(d->loop_start));
    uint32_t end = std::max(kBitsPerPacketHeader, uint32_t(d->loop_end));
    if (d->input_buffer_read_offset != end) return;
    d->input_buffer_read_offset = start;
    loop_skip_pending_ = true;
    if (d->loop_count < 255) d->loop_count--;
  }

  const uint8_t* NextPacket(XMA_CONTEXT_DATA* d, uint32_t index, uint32_t count) {
    if (index < count) return Physical(d->GetCurrentInputBufferAddress()) + index * kBytesPerPacket;
    uint8_t next = d->current_buffer ^ 1;
    if (!d->IsInputBufferValid(next) || !d->GetInputBufferAddress(next)) return nullptr;
    return Physical(d->GetInputBufferAddress(next));
  }

  uint32_t NextPacketReadOffset(uint8_t* buffer, uint32_t index, uint32_t count) {
    while (index < count) {
      uint32_t off = xma::GetPacketFrameOffset(buffer + index * kBytesPerPacket);
      if (off <= kMaxFrameSizeInBits) return index * kBitsPerPacket + off;
      ++index;
    }
    return kBitsPerPacketHeader;
  }

  PacketInfo GetPacketInfo(uint8_t* packet, uint32_t frame_offset) {
    PacketInfo info;
    uint32_t first = xma::GetPacketFrameOffset(packet);
    BitStream s(packet, kBitsPerPacket);
    s.SetOffset(first);
    if (frame_offset < first) {
      info.current_frame = 0;
      info.current_frame_size = first - frame_offset;
    }
    while (s.BitsRemaining() >= kBitsPerFrameHeader) {
      uint64_t size = s.Peek(kBitsPerFrameHeader);
      if (size == 0 || size == xma::kMaxFrameLength) break;
      if (s.offset() == frame_offset) {
        info.current_frame = info.frame_count;
        info.current_frame_size = uint32_t(size);
      }
      info.frame_count++;
      if (size > s.BitsRemaining()) break;
      s.Advance(size - 1);
      if (s.Read(1) == 0) break;
    }
    if (xma::IsPacketXma2Type(packet)) {
      uint8_t count = xma::GetPacketFrameCount(packet);
      if (count > info.frame_count) {
        if (info.current_frame_size == 0) info.current_frame = info.frame_count;
        info.frame_count = count;
      }
    }
    return info;
  }

  void Consume(const XMA_CONTEXT_DATA* d) {
    if (!remaining_subframes_) return;
    const uint8_t total = uint8_t((kBytesPerFrameChannel / kOutputBytesPerBlock) << d->is_stereo);
    if (loop_limit_ > 0 && uint8_t(total - remaining_subframes_) >= loop_limit_) {
      free_blocks_ -= d->output_buffer_padding;
      remaining_subframes_ = 0;
      loop_limit_ = 0;
      return;
    }
    uint8_t sdc = uint8_t(std::max<uint32_t>(1, d->subframe_decode_count));
    int8_t write = int8_t(std::min<uint8_t>(remaining_subframes_, sdc));
    if (loop_limit_ > 0) {
      int8_t until = int8_t(loop_limit_ - (total - remaining_subframes_));
      write = std::min(write, until);
    }
    int8_t read_offset = int8_t(total - remaining_subframes_);
    Write(raw_.data() + kOutputBytesPerBlock * read_offset, uint32_t(write) * kOutputBytesPerBlock);
    int8_t headroom = (remaining_subframes_ - write == 0) ? int8_t(d->output_buffer_padding) : 0;
    free_blocks_ -= write + headroom;
    remaining_subframes_ = uint8_t(remaining_subframes_ - write);
  }

  bool PrepareDecoder(int rate_id, bool stereo) {
    int rate = kSampleRates[std::min(rate_id, 3)];
    int channels = stereo ? 2 : 1;
    if (av_ && av_->sample_rate == rate && av_->channels == channels) return false;
    if (av_) avcodec_free_context(&av_);
    av_ = avcodec_alloc_context3(codec_);
    av_->sample_rate = rate;
    av_->channels = channels;
    av_->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;
    if (avcodec_open2(av_, codec_, nullptr) < 0) {
      REXLOG_ERROR("copa audio: failed to open the XMA decoder");
      avcodec_free_context(&av_);
    }
    return true;
  }

  void Decode(XMA_CONTEXT_DATA* d) {
    if (!d->IsAnyInputBufferValid() || remaining_subframes_ > 0) return;
    if (!d->IsCurrentInputBufferValid()) {
      SwapInput(d);
      if (!d->IsCurrentInputBufferValid()) return;
    }
    uint8_t* input = Physical(d->GetCurrentInputBufferAddress());
    input_.fill(0);
    bool loop_end_frame = false;
    if (d->loop_count > 0)
      loop_end_frame = d->input_buffer_read_offset == std::max(kBitsPerPacketHeader, uint32_t(d->loop_end));
    if (!d->output_buffer_block_count) return;
    if (d->input_buffer_read_offset < kBitsPerPacketHeader) d->input_buffer_read_offset = kBitsPerPacketHeader;
    uint32_t input_size = d->GetCurrentInputBufferPacketCount() * kBytesPerPacket;
    uint32_t packet_count = input_size / kBytesPerPacket;
    if (d->input_buffer_read_offset >= input_size * 8) {
      SwapInput(d);
      return;
    }
    uint32_t packet_index = (d->input_buffer_read_offset >> 3) / kBytesPerPacket;
    uint8_t* packet = input + packet_index * kBytesPerPacket;
    uint32_t first = xma::GetPacketFrameOffset(packet);
    uint32_t relative = d->input_buffer_read_offset % kBitsPerPacket;
    if (relative < first) {
      d->input_buffer_read_offset = packet_index * kBitsPerPacket + first;
      relative = first;
    }
    uint8_t skip = xma::GetPacketSkipCount(packet);
    if (skip == 0xFF) {
      uint32_t next = NextPacketReadOffset(input, packet_index + 1, packet_count);
      if (next == kBitsPerPacketHeader) SwapInput(d);
      d->input_buffer_read_offset = next;
      return;
    }
    PacketInfo info = GetPacketInfo(packet, relative);
    uint32_t next_packet_index = packet_index + skip + 1;
    if (info.current_frame_size == 0) {
      const uint8_t* next = NextPacket(d, next_packet_index, packet_count);
      if (!next) {
        SwapInput(d);
        return;
      }
      std::memcpy(input_.data(), packet + kBytesPerPacketHeader, kBytesPerPacketData);
      std::memcpy(input_.data() + kBytesPerPacketData, next + kBytesPerPacketHeader, kBytesPerPacketData);
      BitStream combined(input_.data(), (kBitsPerPacket - kBitsPerPacketHeader) * 2);
      combined.SetOffset(relative - kBitsPerPacketHeader);
      uint64_t size = combined.Peek(kBitsPerFrameHeader);
      if (size == xma::kMaxFrameLength) {
        d->error_status = 4;
        return;
      }
      info.current_frame_size = uint32_t(size);
    }
    if (info.current_frame_size > kMaxFrameSizeInBits) {
      // Not a frame boundary (corrupt or misaligned data): stop like the decoder's error state.
      d->error_status = 4;
      return;
    }
    BitStream s(input, (packet_index + 1) * kBitsPerPacket);
    s.SetOffset(d->input_buffer_read_offset);
    uint64_t bits = std::min<uint64_t>(s.BitsRemaining(), info.current_frame_size);
    if (bits == 0) {
      SwapInput(d);
      return;
    }
    if (info.IsLastFrameInPacket() && s.BitsRemaining() < info.current_frame_size) {
      const uint8_t* next = NextPacket(d, next_packet_index, packet_count);
      if (!next) {
        d->error_status = 4;
        return;
      }
      std::memcpy(input_.data() + kBytesPerPacketData, next + kBytesPerPacketHeader, kBytesPerPacketData);
    }
    std::memcpy(input_.data(), packet + kBytesPerPacketHeader, kBytesPerPacketData);
    BitStream fs(input_.data(), (kBitsPerPacket - kBitsPerPacketHeader) * 2);
    fs.SetOffset(relative - kBitsPerPacketHeader);
    frame_.fill(0);
    uint32_t padding = uint32_t(fs.Copy(frame_.data() + 1, info.current_frame_size));
    decoded_.fill(0);
    if (PrepareDecoder(d->sample_rate, d->is_stereo)) carry_valid_ = false;
    bool ok = false;
    if (av_) {
      packet_->data = frame_.data();
      packet_->size = int(1 + (padding + info.current_frame_size) / 8 +
                          (((padding + info.current_frame_size) % 8) ? 1 : 0));
      uint32_t padding_end = uint32_t(packet_->size) * 8 - (8 + padding + info.current_frame_size);
      frame_[0] = uint8_t(((padding & 7) << 5) | ((padding_end & 7) << 2));
      int sent = avcodec_send_packet(av_, packet_);
      int received = sent >= 0 ? avcodec_receive_frame(av_, av_frame()) : sent;
      ok = sent >= 0 && received >= 0;
      if (ok) {
        ++g_xma_frames;
        failed_frames_ = 0;
      } else {
        uint64_t n = ++g_xma_errors;
        if (n <= 12)
          REXLOG_WARN("copa xma: decode failed send={} receive={} rate={} stereo={} frame_bits={} "
                      "padding={} packet={} offset={}",
                      sent, received, int(d->sample_rate), int(d->is_stereo),
                      info.current_frame_size, padding, packet_index,
                      uint32_t(d->input_buffer_read_offset));
      }
    }
    if (ok) {
      ConvertFrame(d->is_stereo);
      size_t pad = size_t(kDecoderStartPadding) * kBytesPerSample << d->is_stereo;
      size_t carry_bytes = (size_t(kBytesPerFrameChannel) << d->is_stereo) - pad;
      uint8_t limit = loop_end_frame ? uint8_t((d->loop_subframe_end + 1) << d->is_stereo) : 0;
      uint8_t start_skip = loop_skip_pending_ ? uint8_t(d->loop_subframe_skip << d->is_stereo) : 0;
      loop_skip_pending_ = false;
      if (carry_valid_) {
        std::memcpy(raw_.data(), carry_.data(), carry_bytes);
        std::memcpy(raw_.data() + carry_bytes, decoded_.data(), pad);
        remaining_subframes_ = uint8_t(4 << d->is_stereo);
        loop_limit_ = pending_limit_;
        remaining_subframes_ = uint8_t(remaining_subframes_ - std::min(pending_skip_, remaining_subframes_));
      }
      std::memcpy(carry_.data(), decoded_.data() + pad, carry_bytes);
      carry_valid_ = true;
      pending_limit_ = limit;
      pending_skip_ = start_skip;
    } else {
      carry_valid_ = false;
      ++failed_frames_;
    }
    if (loop_end_frame) {
      UpdateLoop(d);
      return;
    }
    if (!info.IsLastFrameInPacket()) {
      uint32_t next_frame = uint32_t((d->input_buffer_read_offset + bits) % kBitsPerPacket);
      d->input_buffer_read_offset = packet_index * kBitsPerPacket + next_frame;
      return;
    }
    uint32_t next = NextPacketReadOffset(input, next_packet_index, packet_count);
    if (next == kBitsPerPacketHeader) {
      SwapInput(d);
      if (d->IsAnyInputBufferValid()) {
        next = xma::GetPacketFrameOffset(Physical(d->GetCurrentInputBufferAddress()));
        if (next > kMaxFrameSizeInBits) {
          SwapInput(d);
          return;
        }
      }
    }
    d->input_buffer_read_offset = next;
  }

  AVFrame* av_frame() { return frame_av_; }

  void ConvertFrame(bool stereo) {
    auto* out = reinterpret_cast<int16_t*>(decoded_.data());
    const float* ch0 = reinterpret_cast<const float*>(frame_av_->data[0]);
    const float* ch1 = stereo ? reinterpret_cast<const float*>(frame_av_->data[1]) : nullptr;
    uint32_t o = 0;
    for (uint32_t i = 0; i < kSamplesPerFrame; ++i) {
      for (uint32_t c = 0; c <= uint32_t(stereo); ++c) {
        float v = std::clamp(c ? ch1[i] : ch0[i], -1.0f, 1.0f) * 32767.0f;
        out[o++] = int16_t(std::byteswap(uint16_t(int16_t(v))));
      }
    }
  }

  const AVCodec* codec_ = nullptr;
  AVCodecContext* av_ = nullptr;
  AVPacket* packet_ = nullptr;
  AVFrame* frame_av_ = nullptr;
  std::array<uint8_t, kBytesPerPacketData * 2> input_{};
  std::array<uint8_t, 1 + 4096> frame_{};
  std::array<uint8_t, kBytesPerFrameChannel * 2> raw_{}, decoded_{}, carry_{};
  bool carry_valid_ = false;
  uint32_t failed_frames_ = 0;  // consecutive frames the decoder rejected
  uint8_t pending_limit_ = 0, pending_skip_ = 0, remaining_subframes_ = 0, loop_limit_ = 0;
  bool loop_skip_pending_ = false;
  uint8_t* out_base_ = nullptr;
  uint32_t out_capacity_ = 0, out_read_ = 0, out_write_ = 0, out_used_ = 0;
  int32_t free_blocks_ = 0;
};

// ------------------------------------------------------------------------ XMA engine
class XmaEngine {
 public:
  static XmaEngine& Get() {
    static XmaEngine e;
    return e;
  }

  bool Init() {
    std::call_once(once_, [this] {
      auto* audio = copa::NativeAudioSystem::Get();
      if (!audio) return;
      array_physical_ = audio->contexts_physical();
      array_virtual_ = audio->contexts_virtual();
      if (!array_virtual_) return;
      for (uint32_t i = 0; i < copa::NativeAudioSystem::kContextCount; ++i) {
        contexts_[i].guest = array_virtual_ + i * 64;
        if (!contexts_[i].Setup()) {
          REXLOG_ERROR("copa audio: libavcodec XMA decoder unavailable");
          return;
        }
      }
      copa::NativeAudioSystem::Handlers h;
      h.kick = [](uint32_t i) { XmaEngine::Get().KickIndex(i); };
      h.lock = [](uint32_t i) { XmaEngine::Get().LockIndex(i); };
      h.clear = [](uint32_t i) { XmaEngine::Get().ClearIndex(i); };
      audio->SetHandlers(h);
      ready_ = true;
      worker_ = std::thread([this] { Loop(); });
      worker_.detach();
    });
    return ready_;
  }

  // Register-window requests (synchronous, like the hardware interface the game uses).
  void KickIndex(uint32_t i) {
    ++g_xma_kicks;
    auto& c = contexts_[i];
    c.allocated = true;
    c.enabled = true;
    c.Work();  // decode inline; the worker sweeps anything left over
    wake_.notify_all();
  }
  void LockIndex(uint32_t i) {
    auto& c = contexts_[i];
    c.enabled = false;
    std::lock_guard g(c.lock);  // the game edits the context right after locking it
  }
  void ClearIndex(uint32_t i) { contexts_[i].Clear(); }

  uint32_t Allocate() {
    if (!Init()) return 0;
    std::lock_guard g(mutex_);
    for (auto& c : contexts_) {
      if (!c.allocated) {
        c.allocated = true;
        ++g_xma_created;
        c.ResetDecoder();
        return c.guest;
      }
    }
    return 0;
  }

  XmaContext* Find(uint32_t guest) {
    if (!Init() || guest < array_virtual_) return nullptr;
    uint32_t index = (guest - array_virtual_) / 64;
    return index < copa::NativeAudioSystem::kContextCount ? &contexts_[index] : nullptr;
  }

  void Kick(XmaContext* c) {
    ++g_xma_kicks;
    c->enabled = true;
    wake_.notify_all();
  }

 private:
  void Loop() {
    while (true) {
      {
        std::unique_lock l(wake_mutex_);
        wake_.wait_for(l, std::chrono::milliseconds(1));
      }
      for (auto& c : contexts_) c.Work();
    }
  }

  std::once_flag once_;
  bool ready_ = false;
  uint32_t array_physical_ = 0, array_virtual_ = 0;
  std::array<XmaContext, copa::NativeAudioSystem::kContextCount> contexts_;
  std::mutex mutex_, wake_mutex_;
  std::condition_variable wake_;
  std::thread worker_;
};

uint32_t PhysicalOf(uint32_t guest) { return Memory()->GetPhysicalAddress(guest); }
XMA_CONTEXT_DATA LoadContext(uint8_t* base, uint32_t guest) { return XMA_CONTEXT_DATA(copa::GuestPtr(base, guest)); }

// ------------------------------------------------------------------------ render driver
class RenderDriver {
 public:
  static RenderDriver& Get() {
    static RenderDriver d;
    return d;
  }

  uint32_t Register(uint32_t callback, uint32_t argument) {
    std::lock_guard g(mutex_);
    for (uint32_t i = 0; i < kClients; ++i) {
      if (!clients_[i].callback) {
        // The callback receives a pointer to a guest word holding its argument.
        uint32_t wrapped = Memory()->SystemHeapAlloc(4);
        StoreBE(Memory()->virtual_membase(), wrapped, argument);
        clients_[i] = {callback, wrapped};
        Start();
        return 0x41550000u | i;
      }
    }
    return 0;
  }
  void Unregister(uint32_t index) {
    std::lock_guard g(mutex_);
    if (index < kClients) clients_[index] = {};
  }

  // 256 samples x 6 channels, planar big-endian floats (FL FR C LFE SL SR).
  void Submit(const uint8_t* samples) {
    ++g_driver_submits;
    std::vector<float> stereo(kSamples * 2);
    auto ch = [&](int c, int i) {
      uint32_t v;
      std::memcpy(&v, samples + (c * kSamples + i) * 4, 4);
      v = std::byteswap(v);
      float f;
      std::memcpy(&f, &v, 4);
      return f;
    };
    for (int i = 0; i < int(kSamples); ++i) {
      float c = ch(2, i) * 0.7071f, lfe = ch(3, i) * 0.5f;
      stereo[i * 2] = std::clamp(ch(0, i) + c + lfe + ch(4, i) * 0.7071f, -1.0f, 1.0f);
      stereo[i * 2 + 1] = std::clamp(ch(1, i) + c + lfe + ch(5, i) * 0.7071f, -1.0f, 1.0f);
    }
    std::lock_guard g(queue_mutex_);
    queue_.push_back(std::move(stereo));
    queue_cv_.notify_all();
  }

 private:
  static constexpr uint32_t kClients = 8;
  static constexpr uint32_t kSamples = 256;
  struct Client {
    uint32_t callback = 0, argument = 0;
  };

  void Start() {
    if (started_) return;
    started_ = true;
    auto* ks = rex::Runtime::instance()->kernel_state();
    thread_ = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
        ks, 256 * 1024, 0, [this]() { return Loop(); }));
    thread_->set_name("Copa Audio");
    thread_->Create();
  }

  bool InitXAudio() {
#ifndef _WIN32
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return false;
    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, 48000};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream_) return false;
    if (!SDL_ResumeAudioStreamDevice(stream_)) {
      SDL_DestroyAudioStream(stream_);
      stream_ = nullptr;
      return false;
    }
    return true;
#else
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(XAudio2Create(&xaudio_, 0, XAUDIO2_DEFAULT_PROCESSOR))) return false;
    if (FAILED(xaudio_->CreateMasteringVoice(&master_))) return false;
    WAVEFORMATEX f{};
    f.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    f.nChannels = 2;
    f.nSamplesPerSec = 48000;
    f.wBitsPerSample = 32;
    f.nBlockAlign = 8;
    f.nAvgBytesPerSec = 48000 * 8;
    if (FAILED(xaudio_->CreateSourceVoice(&voice_, &f))) return false;
    voice_->Start(0);
    return true;
#endif
  }

  // XAudio2 creation can block indefinitely when the audio endpoint is wedged (seen while
  // a killed instance was still being torn down); never let that stall the game.
  bool InitXAudioWithTimeout() {
#ifndef _WIN32
    return InitXAudio();
#else
    auto done = std::make_shared<std::promise<bool>>();
    auto result = done->get_future();
    std::thread([this, done] { done->set_value(InitXAudio()); }).detach();
    if (result.wait_for(std::chrono::seconds(3)) == std::future_status::ready) return result.get();
    REXLOG_ERROR("copa audio: XAudio2 initialization timed out");
    return false;
#endif
  }

  int Loop() {
    bool audio = InitXAudioWithTimeout();
    if (!audio) REXLOG_ERROR("copa audio: XAudio2 unavailable, audio output disabled");
    auto* dispatcher = rex::Runtime::instance()->function_dispatcher();
    auto* thread = rex::system::XThread::GetCurrentThread();
    std::deque<std::vector<float>> in_flight;
    auto next = std::chrono::steady_clock::now();
    auto next_log = next;
    while (true) {
      if (std::chrono::steady_clock::now() >= next_log) {
        next_log += std::chrono::seconds(2);
        REXLOG_INFO("copa audio stats: xma created={} kicks={} frames={} errors={} driver "
                    "callbacks={} submits={} xaudio={}",
                    g_xma_created.load(), g_xma_kicks.load(), g_xma_frames.load(),
                    g_xma_errors.load(), g_driver_callbacks.load(), g_driver_submits.load(), audio);
        if (auto* a = copa::NativeAudioSystem::Get()) {
          uint32_t in_valid = 0, out_valid = 0, err = 0, loops = 0, busy = 0;
          for (uint32_t i = 0; i < copa::NativeAudioSystem::kContextCount; ++i) {
            XMA_CONTEXT_DATA c(copa::GuestPtr(Memory()->virtual_membase(), a->contexts_virtual() + i * 64));
            in_valid += c.IsAnyInputBufferValid();
            out_valid += c.output_buffer_valid;
            err += c.error_status != 0;
            loops += c.loop_count != 0;
            busy += c.IsAnyInputBufferValid() && c.output_buffer_valid;
          }
          REXLOG_INFO("copa xma contexts: input_valid={} output_valid={} both={} error={} looping={}",
                      in_valid, out_valid, busy, err, loops);
        }
      }
      // Keep ~3 frames (16 ms) queued in XAudio2; without output, pace by the clock.
      uint32_t queued = 0;
      if (audio) {
        // PC settings volume (0 while the settings menu holds the game).
        float volume = pcs::Volume();
#ifdef _WIN32
        if (volume != volume_) master_->SetVolume(volume_ = volume);
        XAUDIO2_VOICE_STATE st;
        voice_->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        queued = st.BuffersQueued;
        while (in_flight.size() > queued) in_flight.pop_front();
#else
        if (volume != volume_) SDL_SetAudioStreamGain(stream_, volume_ = volume);
        const int bytes = SDL_GetAudioStreamQueued(stream_);
        if (bytes < 0) {
          SDL_DestroyAudioStream(stream_);
          stream_ = nullptr;
          audio = false;
          next = std::chrono::steady_clock::now();
        } else {
          queued = (uint32_t(bytes) + kSamples * 8 - 1) / (kSamples * 8);
        }
#endif
      }
      bool need = audio ? queued < 3 : std::chrono::steady_clock::now() >= next;
      if (!need) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      next += std::chrono::microseconds(5333);
      Client clients[kClients];
      {
        std::lock_guard g(mutex_);
        std::copy(std::begin(clients_), std::end(clients_), clients);
      }
      for (auto& c : clients) {
        if (!c.callback) continue;
        uint64_t args[] = {c.argument};
        ++g_driver_callbacks;
        dispatcher->Execute(thread->thread_state(), c.callback, args, 1);
      }
      std::vector<std::vector<float>> frames;
      {
        std::lock_guard g(queue_mutex_);
        while (!queue_.empty()) {
          frames.push_back(std::move(queue_.front()));
          queue_.pop_front();
        }
      }
      if (frames.empty()) frames.emplace_back(kSamples * 2, 0.0f);
      // Several clients submit separately: mix them into one buffer.
      std::vector<float> mixed(kSamples * 2, 0.0f);
      for (auto& f : frames)
        for (size_t i = 0; i < mixed.size(); ++i) mixed[i] += f[i];
      for (auto& v : mixed) v = std::clamp(v, -1.0f, 1.0f);
      if (audio) {
#ifdef _WIN32
        in_flight.push_back(std::move(mixed));
        XAUDIO2_BUFFER b{};
        b.AudioBytes = UINT32(in_flight.back().size() * 4);
        b.pAudioData = reinterpret_cast<const BYTE*>(in_flight.back().data());
        voice_->SubmitSourceBuffer(&b);
#else
        if (!SDL_PutAudioStreamData(stream_, mixed.data(), int(mixed.size() * sizeof(float)))) {
          SDL_DestroyAudioStream(stream_);
          stream_ = nullptr;
          audio = false;
          next = std::chrono::steady_clock::now();
        }
#endif
      }
    }
    return 0;
  }

  std::mutex mutex_, queue_mutex_;
  std::condition_variable queue_cv_;
  Client clients_[kClients];
  std::deque<std::vector<float>> queue_;
  bool started_ = false;
  rex::system::object_ref<rex::system::XHostThread> thread_;
  float volume_ = 1.0f;
#ifdef _WIN32
  IXAudio2* xaudio_ = nullptr;
  IXAudio2MasteringVoice* master_ = nullptr;
  IXAudio2SourceVoice* voice_ = nullptr;
#else
  SDL_AudioStream* stream_ = nullptr;
#endif
};

}  // namespace

// ============================================================================ exports
// Typed access helpers for raw hooks.
#define R(n) ctx.r##n.u32

REX_HOOK_RAW(__imp__XAudioRegisterRenderDriverClient) {
  uint32_t callback_ptr = R(3), driver_ptr = R(4);
  uint32_t callback = LoadBE(base, callback_ptr), argument = LoadBE(base, callback_ptr + 4);
  uint32_t handle = RenderDriver::Get().Register(callback, argument);
  if (!handle) {
    ctx.r3.u64 = 0x8007000E;  // E_OUTOFMEMORY
    return;
  }
  StoreBE(base, driver_ptr, handle);
  ctx.r3.u64 = 0;
}

REX_HOOK_RAW(__imp__XAudioUnregisterRenderDriverClient) {
  RenderDriver::Get().Unregister(R(3) & 0xFFFF);
  ctx.r3.u64 = 0;
}

REX_HOOK_RAW(__imp__XAudioSubmitRenderDriverFrame) {
  RenderDriver::Get().Submit(copa::GuestPtr(base, R(4)));
  ctx.r3.u64 = 0;
}

REX_HOOK_RAW(__imp__XMACreateContext) {
  uint32_t out = R(3);
  uint32_t c = XmaEngine::Get().Allocate();
  StoreBE(base, out, c);
  ctx.r3.u64 = c ? 0 : 0xC0000017;  // STATUS_NO_MEMORY
}

REX_HOOK_RAW(__imp__XMAReleaseContext) {
  if (auto* c = XmaEngine::Get().Find(R(3))) c->Release();
  ctx.r3.u64 = 0;
}

REX_HOOK_RAW(__imp__XMAInitializeContext) {
  uint32_t guest = R(3), init = R(4);
  auto be = [&](uint32_t off) { return LoadBE(base, init + off); };
  std::memset(copa::GuestPtr(base, guest), 0, 64);
  XMA_CONTEXT_DATA c(copa::GuestPtr(base, guest));
  c.input_buffer_0_ptr = be(0) ? PhysicalOf(be(0)) : 0;
  c.input_buffer_0_packet_count = be(4);
  c.input_buffer_1_ptr = be(8) ? PhysicalOf(be(8)) : 0;
  c.input_buffer_1_packet_count = be(12);
  c.input_buffer_read_offset = be(16);
  c.output_buffer_ptr = PhysicalOf(be(20));
  c.output_buffer_block_count = be(24);
  c.subframe_decode_count = be(32);
  c.is_stereo = be(36) >= 1;
  c.sample_rate = be(40);
  c.loop_start = be(44);
  c.loop_end = be(48);
  c.loop_count = *copa::GuestPtr(base, init + 52);
  c.loop_subframe_end = *copa::GuestPtr(base, init + 53);
  c.loop_subframe_skip = *copa::GuestPtr(base, init + 54);
  c.Store(copa::GuestPtr(base, guest));
  if (auto* x = XmaEngine::Get().Find(guest)) x->ResetDecoder();
  ctx.r3.u64 = 0;
}

REX_HOOK_RAW(__imp__XMASetLoopData) {
  uint32_t guest = R(3), loop = R(4);
  XMA_CONTEXT_DATA c(copa::GuestPtr(base, guest));
  XMA_CONTEXT_DATA l(copa::GuestPtr(base, loop));
  c.loop_start = l.loop_start;
  c.loop_end = l.loop_end;
  c.loop_count = l.loop_count;
  c.loop_subframe_end = l.loop_subframe_end;
  c.loop_subframe_skip = l.loop_subframe_skip;
  c.Store(copa::GuestPtr(base, guest));
  ctx.r3.u64 = 0;
}

#define XMA_GETTER(name, field)                         \
  REX_HOOK_RAW(__imp__##name) {                         \
    XMA_CONTEXT_DATA c(copa::GuestPtr(base, R(3)));                    \
    ctx.r3.u64 = uint32_t(c.field);                     \
  }
#define XMA_SETTER(name, field)                         \
  REX_HOOK_RAW(__imp__##name) {                         \
    uint32_t guest = R(3);                              \
    XMA_CONTEXT_DATA c(copa::GuestPtr(base, guest));                   \
    c.field = R(4);                                     \
    c.Store(copa::GuestPtr(base, guest));                              \
    ctx.r3.u64 = 0;                                     \
  }
#define XMA_VALIDATE(name, field)                       \
  REX_HOOK_RAW(__imp__##name) {                         \
    uint32_t guest = R(3);                              \
    XMA_CONTEXT_DATA c(copa::GuestPtr(base, guest));                   \
    c.field = 1;                                        \
    c.Store(copa::GuestPtr(base, guest));                              \
    ctx.r3.u64 = 0;                                     \
  }

XMA_GETTER(XMAGetInputBufferReadOffset, input_buffer_read_offset)
XMA_SETTER(XMASetInputBufferReadOffset, input_buffer_read_offset)
XMA_GETTER(XMAIsInputBuffer0Valid, input_buffer_0_valid)
XMA_VALIDATE(XMASetInputBuffer0Valid, input_buffer_0_valid)
XMA_GETTER(XMAIsInputBuffer1Valid, input_buffer_1_valid)
XMA_VALIDATE(XMASetInputBuffer1Valid, input_buffer_1_valid)
XMA_GETTER(XMAIsOutputBufferValid, output_buffer_valid)
XMA_VALIDATE(XMASetOutputBufferValid, output_buffer_valid)
XMA_GETTER(XMAGetOutputBufferReadOffset, output_buffer_read_offset)
XMA_SETTER(XMASetOutputBufferReadOffset, output_buffer_read_offset)
XMA_GETTER(XMAGetOutputBufferWriteOffset, output_buffer_write_offset)
XMA_GETTER(XMAGetPacketMetadata, packet_metadata)

REX_HOOK_RAW(__imp__XMASetInputBuffer0) {
  uint32_t guest = R(3);
  XMA_CONTEXT_DATA c(copa::GuestPtr(base, guest));
  c.input_buffer_0_ptr = PhysicalOf(R(4));
  c.input_buffer_0_packet_count = R(5);
  c.Store(copa::GuestPtr(base, guest));
  ctx.r3.u64 = 0;
}

REX_HOOK_RAW(__imp__XMASetInputBuffer1) {
  uint32_t guest = R(3);
  XMA_CONTEXT_DATA c(copa::GuestPtr(base, guest));
  c.input_buffer_1_ptr = PhysicalOf(R(4));
  c.input_buffer_1_packet_count = R(5);
  c.Store(copa::GuestPtr(base, guest));
  ctx.r3.u64 = 0;
}

REX_HOOK_RAW(__imp__XMAEnableContext) {
  if (auto* c = XmaEngine::Get().Find(R(3))) XmaEngine::Get().Kick(c);
  ctx.r3.u64 = 0;
}

REX_HOOK_RAW(__imp__XMADisableContext) {
  if (auto* c = XmaEngine::Get().Find(R(3))) {
    c->enabled = false;
    std::lock_guard g(c->lock);  // waits for an in-progress decode
  }
  ctx.r3.u64 = 0;
}

REX_HOOK_RAW(__imp__XMABlockWhileInUse) {
  if (auto* c = XmaEngine::Get().Find(R(3))) std::lock_guard g(c->lock);
  ctx.r3.u64 = 0;
}
