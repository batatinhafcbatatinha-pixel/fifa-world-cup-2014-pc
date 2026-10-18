#include "pc_audio.h"
#include <rex/cvar.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unordered_map>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#endif

REXCVAR_DEFINE_INT32(pc_volume, 100, "Audio", "Volume das sessoes de audio deste processo (0-100)")
    .range(0, 100)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

struct CopaPcAudio::Impl {
  mutable std::mutex mutex;
  std::condition_variable wake;
  bool stopping = false;
  std::string status = "Aguardando sessao de audio deste jogo...";
  std::thread worker;
  Impl() : worker([this] { Run(); }) {}
  ~Impl() {
    { std::lock_guard lock(mutex); stopping = true; }
    wake.notify_one();
    if (worker.joinable()) worker.join();
  }
  void Publish(std::string value) {
    std::lock_guard lock(mutex);
    status = std::move(value);
  }
#ifdef _WIN32
  using VolumeMap = std::unordered_map<std::wstring, int>;
  static std::string Error(HRESULT hr) {
    char code[16];
    std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(hr));
    return std::string("Falha no volume da sessao: ") + code;
  }
  void Poll(VolumeMap& applied) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IMMDeviceEnumerator> devices;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&devices));
    if (FAILED(hr)) { Publish(Error(hr)); return; }
    ComPtr<IMMDeviceCollection> outputs;
    hr = devices->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &outputs);
    if (FAILED(hr)) { Publish(Error(hr)); return; }
    UINT count = 0;
    hr = outputs->GetCount(&count);
    if (FAILED(hr)) { Publish(Error(hr)); return; }
    const int requested = std::clamp(rex::cvar::Query<int32_t>("pc_volume"), 0, 100);
    VolumeMap alive;
    int own_sessions = 0;
    HRESULT failure = S_OK;
    float actual = 0.0f;
    bool muted = false;
    for (UINT d = 0; d < count; ++d) {
      ComPtr<IMMDevice> device;
      hr = outputs->Item(d, &device);
      if (FAILED(hr)) { failure = hr; continue; }
      ComPtr<IAudioSessionManager2> manager;
      hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                             reinterpret_cast<void**>(manager.GetAddressOf()));
      if (FAILED(hr)) { failure = hr; continue; }
      ComPtr<IAudioSessionEnumerator> sessions;
      hr = manager->GetSessionEnumerator(&sessions);
      if (FAILED(hr)) { failure = hr; continue; }
      int session_count = 0;
      hr = sessions->GetCount(&session_count);
      if (FAILED(hr)) { failure = hr; continue; }
      for (int s = 0; s < session_count; ++s) {
        ComPtr<IAudioSessionControl> control;
        hr = sessions->GetSession(s, &control);
        if (FAILED(hr)) { failure = hr; continue; }
        ComPtr<IAudioSessionControl2> session;
        if (FAILED(control.As(&session))) continue;
        DWORD pid = 0;
        // S_NO_SINGLE_PROCESS is deliberately excluded: never touch shared or
        // foreign sessions, endpoint master volume, or other applications.
        if (session->GetProcessId(&pid) != S_OK || pid != GetCurrentProcessId()) continue;
        AudioSessionState state;
        hr = session->GetState(&state);
        if (FAILED(hr)) { failure = hr; continue; }
        if (state == AudioSessionStateExpired) continue;
        ++own_sessions;
        LPWSTR raw_id = nullptr;
        hr = session->GetSessionInstanceIdentifier(&raw_id);
        if (FAILED(hr)) { failure = hr; continue; }
        const std::wstring id = raw_id ? raw_id : L"";
        CoTaskMemFree(raw_id);
        if (id.empty()) { failure = E_FAIL; continue; }
        ComPtr<ISimpleAudioVolume> volume;
        hr = session.As(&volume);
        if (FAILED(hr)) { failure = hr; continue; }
        const auto previous = applied.find(id);
        if (previous == applied.end() || previous->second != requested) {
          hr = volume->SetMasterVolume(float(requested) / 100.0f, nullptr);
          if (FAILED(hr)) { failure = hr; continue; }
        }
        alive.emplace(id, requested);
        hr = volume->GetMasterVolume(&actual);
        if (FAILED(hr)) { failure = hr; continue; }
        BOOL session_mute = FALSE;
        hr = volume->GetMute(&session_mute);
        if (FAILED(hr)) { failure = hr; continue; }
        muted = muted || !!session_mute;
      }
    }
    applied = std::move(alive);
    if (FAILED(failure)) Publish(Error(failure));
    else if (!own_sessions) Publish("Aguardando sessao de audio deste jogo...");
    else Publish("Volume aplicado ao jogo. Sessao: " + std::to_string(int(actual * 100.0f + 0.5f)) +
                 "%" + (muted ? " (silenciada no Windows)" : ""));
  }
#endif
  void Run() {
#ifdef _WIN32
    // All COM objects stay on this worker; no UI-thread apartment assumptions.
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) { Publish(Error(initialized)); return; }
    {
      VolumeMap applied;
      for (;;) {
        try { Poll(applied); }
        catch (const std::exception& e) { Publish(std::string("Falha no volume: ") + e.what()); }
        std::unique_lock lock(mutex);
        if (wake.wait_for(lock, std::chrono::seconds(1), [this] { return stopping; })) break;
      }
    }
    CoUninitialize();
#else
    Publish("Controle de volume de sessao disponivel apenas no Windows.");
#endif
  }
};
CopaPcAudio::CopaPcAudio() : impl_(std::make_unique<Impl>()) {}
CopaPcAudio::~CopaPcAudio() = default;
std::string CopaPcAudio::Status() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->status;
}
