#pragma once
#include <windows.h>
#include <vector>
#include <string>

class CSettings
{
  public:
    struct DisplayMode
    {
      unsigned width;
      unsigned height;
      unsigned refresh;
      bool     preferred;
    };
    typedef std::vector<DisplayMode> DisplayModes;

    CSettings();

    void LoadModes();
    const DisplayModes& GetDisplayModes() { return m_displayModes; }
    void SetExtraMode(const DisplayMode & mode);
    bool GetExtraMode(DisplayMode & mode);

  private:
    DisplayModes m_displayModes;

    bool ReadModesValue(std::vector<std::wstring> &out) const;
    bool ParseModeString(const std::wstring& in, DisplayMode& out);
};

extern CSettings g_settings;