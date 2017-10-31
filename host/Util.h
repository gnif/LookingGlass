#pragma once
#include <string>

#include "common\debug.h"

static class Util
{
public:
  static std::string GetSystemRoot()
  {
    std::string defaultPath;

    size_t pathSize;
    char *libPath;

    if (_dupenv_s(&libPath, &pathSize, "SystemRoot") != 0)
    {
      DEBUG_ERROR("Unable to get the SystemRoot environment variable");
      return defaultPath;
    }

    if (!pathSize)
    {
      DEBUG_ERROR("The SystemRoot environment variable is not set");
      return defaultPath;
    }
#ifdef _WIN64
    defaultPath = std::string(libPath) + "\\System32";
#else
    if (IsWow64())
    {
      defaultPath = std::string(libPath) + "\\Syswow64";
    }
    else
    {
      defaultPath = std::string(libPath) + "\\System32";
    }
#endif
    return defaultPath;
  }
};