/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#pragma once

#include <Windows.h>
#include <stdint.h>
#include <vector>

#include "CSettings.h"

class CEdid
{
public:
  void Build(const CSettings::DisplayModes& modes);

  const BYTE* Data() const { return m_data.empty() ? nullptr : m_data.data(); }
  UINT Size() const { return (UINT)m_data.size(); }

private:
  std::vector<BYTE> m_data;

  static void SetChecksum(BYTE* block);
  static bool WriteDetailedTiming(BYTE* dtd, const CSettings::DisplayMode& mode);
  static void WriteMonitorName(BYTE* desc, const char* name);
};
