// PC options backed by the installed ReXGlue runtime (F2).
#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <rex/ui/imgui_dialog.h>

class CopaPcAudio;

class CopaPcMenu final : public rex::ui::ImGuiDialog {
 public:
  CopaPcMenu(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path);
  ~CopaPcMenu() override;
 private:
  void Toggle();
  void Hide();
  void OnDraw(ImGuiIO& io) override;
  void Set(const char* name, const std::string& value);
  void BoolOption(const char* label, const char* name);
  void Save();
  std::filesystem::path config_path_;
  std::unique_ptr<CopaPcAudio> audio_;
  bool visible_ = false;
  bool set_failed_ = false;
  std::string status_;
};
