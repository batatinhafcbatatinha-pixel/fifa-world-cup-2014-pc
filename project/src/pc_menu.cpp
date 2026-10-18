// Native PC overlay; no guest-memory patches and no fictitious settings.
#include "pc_menu.h"
#include "pc_audio.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <cstdlib>
#include <rex/ui/keybinds.h>
#include <imgui.h>
#include <algorithm>
#include <exception>
#include <utility>
#include <fstream>
#include <iterator>
#include <chrono>
#include <sstream>
#include <stdexcept>
#include <array>
#include <string_view>
#include <toml++/toml.hpp>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
// Only this menu's settings and explicit runtime key remaps may be written.
// CLI/environment options (especially diagnostic paths) are session overrides.
bool IsPcMenuSetting(std::string_view name) {
  constexpr std::array<std::string_view, 16> editable = {
      "fullscreen", "vsync", "resolution", "video_mode_width", "video_mode_height",
      "window_width", "window_height", "resolution_scale", "draw_resolution_scale_x",
      "draw_resolution_scale_y", "anisotropic_override", "audio_mute", "user_language",
      "vibration", "mnk_mode", "pc_volume"};
  return std::find(editable.begin(), editable.end(), name) != editable.end();
}
bool IsRuntimeRemap(const rex::cvar::FlagEntry& entry) {
  return entry.category == "Input/Keybinds/Controller" || entry.category == "Keybinds";
}
// Preserve unknown settings and the original nested layout recognized by the SDK.
bool UpdateExisting(toml::table& table, const std::string& prefix,
                    const std::string& name, const toml::node& replacement) {
  bool found = false;
  for (auto&& [key, node] : table) {
    const auto full = prefix.empty() ? std::string(key.str())
                                    : prefix + "_" + std::string(key.str());
    if (auto* child = node.as_table()) {
      found = UpdateExisting(*child, full, name, replacement) || found;
    } else if (full == name) {
      table.insert_or_assign(key.str(), replacement);
      found = true;
    }
  }
  return found;
}
}


CopaPcMenu::CopaPcMenu(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path)
    : ImGuiDialog(drawer), config_path_(std::move(config_path)),
      audio_(std::make_unique<CopaPcAudio>()) {
  drawer->RemoveDialog(this);
  rex::ui::RegisterBind("bind_copa_pc_menu", "F2", "Abrir menu PC da Copa 2014",
                        [this] { Toggle(); });
  if (const char* open = std::getenv("COPA_PC_MENU"); open && *open == '1') Toggle();
}
CopaPcMenu::~CopaPcMenu() { rex::ui::UnregisterBind("bind_copa_pc_menu"); }
void CopaPcMenu::Toggle() {
  REXLOG_INFO("[pc-menu] {}", visible_ ? "close" : "open");
  if (visible_) Hide();
  else { visible_ = true; imgui_drawer()->AddDialog(this); }
}
void CopaPcMenu::Hide() {
  visible_ = false;
  imgui_drawer()->RemoveDialog(this);
}
void CopaPcMenu::Set(const char* name, const std::string& value) {
  const auto* entry = rex::cvar::GetFlagInfo(name);
  // RequiresRestart is accepted by the SDK and marked pending; InitOnly is
  // intentionally locked. Never report or persist a rejected setter as applied.
  if (!IsPcMenuSetting(name) || !entry || entry->lifecycle == rex::cvar::Lifecycle::kInitOnly ||
      !rex::cvar::SetFlagByName(name, value)) {
    if (!set_failed_) status_ = "Alteracao incompleta. Falha em: ";
    else status_ += ", ";
    status_ += name;
    set_failed_ = true;
  } else if (!set_failed_) {
    const auto* applied = rex::cvar::GetFlagInfo(name);
    status_ = applied && applied->lifecycle == rex::cvar::Lifecycle::kRequiresRestart
        ? "Alterado para o proximo inicio. Use Salvar e reinicie o jogo."
        : "Alterado. Use Salvar para manter no proximo inicio.";
  }
}
void CopaPcMenu::BoolOption(const char* label, const char* name) {
  const auto* entry = rex::cvar::GetFlagInfo(name);
  if (!entry) return;
  const bool locked = entry->lifecycle == rex::cvar::Lifecycle::kInitOnly;
  bool value = rex::cvar::Query<bool>(name);
  ImGui::BeginDisabled(locked);
  if (ImGui::Checkbox(label, &value)) Set(name, value ? "true" : "false");
  ImGui::EndDisabled();
}
void CopaPcMenu::Save() {
  std::filesystem::path temporary;
  try {
    toml::table document;
    const bool existed = std::filesystem::exists(config_path_);
    // Invalid existing TOML is never silently discarded.
    if (existed) document = toml::parse_file(config_path_.string());
    for (const auto& entry : rex::cvar::GetRegistry()) {
      // Preserve existing config entries verbatim at the value level. Only
      // overwrite user changes made during this session, never CLI/env values.
      if (entry.type == rex::cvar::FlagType::Command ||
          entry.source != rex::cvar::Source::kRuntime ||
          (!IsPcMenuSetting(entry.name) && !IsRuntimeRemap(entry))) continue;
      const auto value = entry.getter();
      toml::table single;
      // Parsing scalar values preserves TOML numeric/boolean types; strings are
      // inserted as strings so quotes, backslashes and newlines are escaped.
      if (entry.type == rex::cvar::FlagType::String) single.insert("value", value);
      else single = toml::parse("value = " + value);
      const auto* node = single.get("value");
      if (!node) throw std::runtime_error("Valor invalido: " + entry.name);
      if (!UpdateExisting(document, "", entry.name, *node))
        document.insert_or_assign(entry.name, *node);
    }
    std::ostringstream buffer;
    buffer << "# Configuracao PC da Copa 2014 / ReXGlue\n" << document;
    const std::string content = buffer.str();
    // Verify generated TOML before touching the destination.
    (void)toml::parse(content);
    temporary = config_path_;
    temporary += ".tmp-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    {
      std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
      file.exceptions(std::ios::badbit | std::ios::failbit);
      file.write(content.data(), static_cast<std::streamsize>(content.size()));
      file.flush();
      file.close();
    }
    if (existed) {
      auto backup = config_path_;
      backup += ".bak";
      std::filesystem::copy_file(config_path_, backup, std::filesystem::copy_options::overwrite_existing);
    }
#ifdef _WIN32
    // Both files are siblings on the same volume; replace without a delete gap.
    const bool committed = existed
        ? !!ReplaceFileW(config_path_.c_str(), temporary.c_str(), nullptr, 0, nullptr, nullptr)
        : !!MoveFileExW(temporary.c_str(), config_path_.c_str(), MOVEFILE_WRITE_THROUGH);
    if (!committed) throw std::runtime_error("Falha na substituicao atomica: " + std::to_string(GetLastError()));
#else
    std::filesystem::rename(temporary, config_path_);
#endif
    temporary.clear();
    status_ = "Configuracao salva. Opcoes marcadas exigem reiniciar o jogo.";
  } catch (const std::exception& e) {
    status_ = std::string("Configuracao original preservada. Erro ao salvar: ") + e.what();
    if (!temporary.empty()) {
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
    }
  }
}
void CopaPcMenu::OnDraw(ImGuiIO&) {
  if (!visible_) return;
  set_failed_ = false;
  const auto* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowSize(ImVec2(std::min(920.0f, viewport->WorkSize.x - 40.0f),
                                  std::min(650.0f, viewport->WorkSize.y - 40.0f)),
                           ImGuiCond_Appearing);
  ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  bool open = true;
  if (ImGui::Begin("Copa 2014 | Opcoes de PC (F2)", &open, ImGuiWindowFlags_NoCollapse)) {
    ImGui::TextWrapped("F2: menu PC   |   F4: configuracoes avancadas   |   F3: desempenho");
    ImGui::TextWrapped("O jogo continua executando enquanto este menu esta aberto.");
    if (ImGui::BeginTabBar("copa_pc_tabs")) {
      if (ImGui::BeginTabItem("Imagem")) {
        BoolOption("Tela cheia", "fullscreen");
        BoolOption("VSync", "vsync");
        ImGui::TextWrapped("Alt+Enter tambem alterna tela cheia. Resolucao interna exige reiniciar.");
        const char* sizes[] = {"1280 x 720", "1920 x 1080", "2560 x 1440", "3840 x 2160"};
        const char* presets[] = {"720p", "1080p", "1440p", "4k"};
        const int widths[] = {1280, 1920, 2560, 3840};
        const int heights[] = {720, 1080, 1440, 2160};
        int current = -1;
        const int w = rex::cvar::Query<int32_t>("video_mode_width");
        const int h = rex::cvar::Query<int32_t>("video_mode_height");
        for (int i = 0; i < 4; ++i) if (w == widths[i] && h == heights[i]) current = i;
        if (ImGui::BeginCombo("Resolucao de video [reiniciar]", current >= 0 ? sizes[current] : "Personalizada")) {
          for (int i = 0; i < 4; ++i) if (ImGui::Selectable(sizes[i], current == i)) {
            Set("resolution", presets[i]);
            Set("video_mode_width", std::to_string(widths[i]));
            Set("video_mode_height", std::to_string(heights[i]));
            Set("window_width", std::to_string(widths[i]));
            Set("window_height", std::to_string(heights[i]));
          }
          ImGui::EndCombo();
        }
        if (rex::cvar::GetFlagInfo("draw_resolution_scale_x")) {
          int scale = rex::cvar::Query<int32_t>("draw_resolution_scale_x");
          if (ImGui::SliderInt("Escala de renderizacao [reiniciar]", &scale, 1, 4, "%dx")) {
            Set("resolution_scale", std::to_string(scale));
            Set("draw_resolution_scale_x", std::to_string(scale));
            Set("draw_resolution_scale_y", std::to_string(scale));
          }
          ImGui::TextWrapped("Escala aumenta o custo da GPU; a resolucao de video e a escala sao ajustes distintos.");
        }
        if (rex::cvar::GetFlagInfo("anisotropic_override")) {
          const char* modes[] = {"Original do jogo", "Desativado", "1x", "2x", "4x", "8x", "16x"};
          int af = std::clamp(rex::cvar::Query<int32_t>("anisotropic_override") + 1, 0, 6);
          if (ImGui::Combo("Filtro anisotropico", &af, modes, 7)) Set("anisotropic_override", std::to_string(af - 1));
        }
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Som e idioma")) {
        BoolOption("Silenciar audio", "audio_mute");
        int volume = rex::cvar::Query<int32_t>("pc_volume");
        if (ImGui::SliderInt("Volume do jogo", &volume, 0, 100, "%d%%"))
          Set("pc_volume", std::to_string(volume));
        ImGui::TextWrapped("%s", audio_->Status().c_str());
        ImGui::TextWrapped("Aplicacao em ate 1 segundo. Use Salvar para manter o volume.");
        if (rex::cvar::GetFlagInfo("user_language")) {
          const auto lang = rex::cvar::Query<uint32_t>("user_language");
          if (ImGui::BeginCombo("Idioma solicitado ao jogo [reiniciar]", lang == 9 ? "Portugues" : lang == 1 ? "English" : "Outro")) {
            if (ImGui::Selectable("Portugues", lang == 9)) Set("user_language", "9");
            if (ImGui::Selectable("English", lang == 1)) Set("user_language", "1");
            ImGui::EndCombo();
          }
          ImGui::TextWrapped("A disponibilidade de textos e narracao depende dos recursos do jogo.");
        }
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Controles")) {
        BoolOption("Vibracao do controle", "vibration");
        BoolOption("Jogar pelo teclado", "mnk_mode");
        ImGui::TextWrapped("Controle conectado: reconhecimento automatico pelo SDL. F4 permite remapear as teclas.");
        ImGui::Separator();
        for (const auto& entry : rex::cvar::GetRegistry()) {
          if (entry.category != "Input/Keybinds/Controller") continue;
          ImGui::Text("%s: %s", entry.description.c_str(), entry.getter().c_str());
        }
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
    ImGui::Separator();
    if (ImGui::Button("Salvar configuracao")) Save();
    ImGui::SameLine();
    if (ImGui::Button("Voltar ao jogo")) open = false;
    if (!status_.empty()) ImGui::TextWrapped("%s", status_.c_str());
    ImGui::TextWrapped("Arquivo: %s", config_path_.string().c_str());
  }
  ImGui::End();
  if (!open) Hide();
}
