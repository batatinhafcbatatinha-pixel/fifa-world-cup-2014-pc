#pragma once
#include <array>
#include <chrono>
#include <cstdlib>
#include <rex/logging.h>

// Called under the renderer API mutex. Inclusive CPU times; no GPU query stalls.
namespace copa {
struct RenderProfile {
  enum Phase { Draw, Shader, Texture, Vertex, Indices, Resolve, WriteBack, Present, Count };
  bool enabled = std::getenv("COPA_PROFILE") != nullptr;
  std::array<double, Count> ms{};
  std::array<unsigned long long, Count> calls{};
  unsigned frames = 0;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  // Worst frame of the window: interval since the previous Frame() and its own phase times.
  std::array<double, Count> cur{}, worst_ms{};
  double worst = 0;
  std::chrono::steady_clock::time_point last = start;
  static RenderProfile& Get() { static RenderProfile p; return p; }
  void Frame() {
    if (!enabled) return;
    ++frames;
    auto now = std::chrono::steady_clock::now();
    double interval = std::chrono::duration<double, std::milli>(now - last).count();
    last = now;
    if (interval > worst) { worst = interval; worst_ms = cur; }
    cur.fill(0);
    double elapsed = std::chrono::duration<double>(now - start).count();
    if (elapsed < 5) return;
    REXLOG_INFO("copa profile: frames={} fps={:.1f} ms/frame draw={:.3f} shader={:.3f} texture={:.3f} vertex={:.3f} indices={:.3f} resolve={:.3f} writeback={:.3f} present={:.3f} calls/frame shader={:.1f} texture={:.1f} vertex={:.1f}",
      frames, frames / elapsed, ms[Draw]/frames, ms[Shader]/frames, ms[Texture]/frames,
      ms[Vertex]/frames, ms[Indices]/frames, ms[Resolve]/frames, ms[WriteBack]/frames,
      ms[Present]/frames, double(calls[Shader])/frames, double(calls[Texture])/frames,
      double(calls[Vertex])/frames);
    REXLOG_INFO("copa profile worst frame {:.1f} ms: draw={:.2f} shader={:.2f} texture={:.2f} vertex={:.2f} indices={:.2f} resolve={:.2f} writeback={:.2f} present={:.2f}",
      worst, worst_ms[Draw], worst_ms[Shader], worst_ms[Texture], worst_ms[Vertex], worst_ms[Indices],
      worst_ms[Resolve], worst_ms[WriteBack], worst_ms[Present]);
    worst = 0;
    ms.fill(0); calls.fill(0); frames = 0; start = now;
  }
};
struct RenderPhase {
  RenderProfile::Phase phase;
  bool enabled;
  std::chrono::steady_clock::time_point start;
  explicit RenderPhase(RenderProfile::Phase p) : phase(p), enabled(RenderProfile::Get().enabled) {
    if (enabled) start = std::chrono::steady_clock::now();
  }
  ~RenderPhase() {
    if (!enabled) return;
    auto& p = RenderProfile::Get();
    double d = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count();
    p.ms[phase] += d;
    p.cur[phase] += d;
    ++p.calls[phase];
  }
};
}
