/*
KVMGFX Client - A KVM Client for VGA Passthrough
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#pragma once
#include <string>

#include "common\debug.h"

class Util
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