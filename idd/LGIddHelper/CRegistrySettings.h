#pragma once

#include <optional>
#include <vector>
#include <string>
#include <windows.h>

struct DisplayMode {
  unsigned width;
  unsigned height;
  unsigned refresh;
  bool     preferred;

  std::wstring toString();
};

class CRegistrySettings {
  HKEY hKey;

public:
  CRegistrySettings();
  ~CRegistrySettings();

  LSTATUS open();
  bool isOpen() { return !!hKey; }

  std::optional<std::vector<DisplayMode>> getModes();
  LSTATUS setModes(const std::vector<DisplayMode> &modes);
};
