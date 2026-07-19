/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#pragma once

#include <Windows.h>
#include <stdint.h>
#include <vector>

#include "CSettings.h"

class CEdid
{
public:
  struct Timing
  {
    DWORD  hActive;
    DWORD  hBlank;
    DWORD  hFront;
    DWORD  hSync;
    DWORD  vActive;
    DWORD  vBlank;
    DWORD  vFront;
    DWORD  vSync;
    UINT64 pixelClock;
  };

  void Build(const CSettings::DisplayModes& modes, bool hdr);

  static bool GetTiming(Timing& timing,
    const CSettings::DisplayMode& mode);

  const BYTE* Data() const { return m_data.empty() ? nullptr : m_data.data(); }
  UINT Size() const { return (UINT)m_data.size(); }

private:
  std::vector<BYTE> m_data;

  static void SetChecksum(BYTE* block);
  static bool WriteDetailedTiming(BYTE* dtd, const CSettings::DisplayMode& mode);
  static void WriteMonitorName(BYTE* desc, const char* name);
};
