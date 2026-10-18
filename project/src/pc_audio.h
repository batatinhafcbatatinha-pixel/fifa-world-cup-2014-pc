// Windows Core Audio volume for this executable's sessions only.
#pragma once
#include <memory>
#include <string>
class CopaPcAudio {
 public:
  CopaPcAudio();
  ~CopaPcAudio();
  CopaPcAudio(const CopaPcAudio&) = delete;
  CopaPcAudio& operator=(const CopaPcAudio&) = delete;
  std::string Status() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
