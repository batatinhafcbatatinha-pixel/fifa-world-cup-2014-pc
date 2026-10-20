// Offline check of the shader translator: translates Xenos microcode files (big-endian
// dwords, named vs_*.bin / ps_*.bin) and compiles the HLSL with d3dcompiler.
// Does not run the game.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3dcompiler.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include "native/shader_translator.h"

int main(int argc, char** argv) {
  auto compile = reinterpret_cast<decltype(&D3DCompile)>(
      GetProcAddress(LoadLibraryW(L"d3dcompiler_47.dll"), "D3DCompile"));
  int failures = 0, total = 0;
  for (int a = 1; a < argc; ++a) {
    for (auto& e : std::filesystem::directory_iterator(argv[a])) {
      auto name = e.path().filename().string();
      if (e.path().extension() != ".bin") continue;
      bool ps = name.rfind("ps_", 0) == 0;
      std::ifstream f(e.path(), std::ios::binary);
      std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
      std::vector<uint32_t> dw(bytes.size() / 4);
      for (size_t i = 0; i < dw.size(); ++i)
        dw[i] = uint32_t(bytes[i * 4] << 24 | bytes[i * 4 + 1] << 16 | bytes[i * 4 + 2] << 8 |
                         bytes[i * 4 + 3]);
      copa::ShaderKey key;
      auto t = copa::TranslateShader(ps ? copa::ShaderStage::kPixel : copa::ShaderStage::kVertex,
                                        dw.data(), uint32_t(dw.size()), key);
      ++total;
      if (!t.ok) {
        printf("%s: translation failed: %s\n", name.c_str(), t.error.c_str());
        ++failures;
        continue;
      }
      ID3DBlob *code = nullptr, *err = nullptr;
      HRESULT hr = compile(t.hlsl.data(), t.hlsl.size(), name.c_str(), nullptr, nullptr, "main",
                           ps ? "ps_5_0" : "vs_5_0", 0, 0, &code, &err);
      if (FAILED(hr)) {
        printf("%s: compile failed:\n%s\n", name.c_str(), err ? (const char*)err->GetBufferPointer() : "");
        std::ofstream(e.path().string() + ".hlsl") << t.hlsl;
        ++failures;
      } else {
        printf("%s: ok (%zu bytes DXBC, vfetch %zu, tex mask %08X, outputs %X)\n", name.c_str(),
               code->GetBufferSize(), t.vfetch_slots.size(), t.tfetch_mask, t.color_outputs);
        if (argc > 2 && std::string(argv[argc - 1]) == "--dump") printf("%s\n", t.hlsl.c_str());
      }
      if (code) code->Release();
      if (err) err->Release();
    }
  }
  printf("%d/%d ok\n", total - failures, total);
  return failures ? 1 : 0;
}
