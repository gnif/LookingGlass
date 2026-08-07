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

#include "config/CSettings.h"
#include "display/CEdid.h"
#include "display/IddCxCompat.h"
#include "transport/FrameMemoryLimits.h"

#include <stddef.h>
#include <stdint.h>
#include <vector>

class CDisplayConfiguration
{
public:
  enum class ResolutionStatus
  {
    SUCCESS,
    INVALID,
    TOO_LARGE,
    SETTINGS_FAILED,
    MODES_FAILED,
  };

  struct ResolutionResult
  {
    ResolutionStatus       status      = ResolutionStatus::INVALID;
    CSettings::DisplayMode mode        = {};
    uint32_t               requiredMiB = 0;
  };

  struct Description
  {
    size_t            modeCount = 0;
    std::vector<BYTE> edid;
  };

private:
  CSettings& m_settings;

  // Registry-backed changes are serialized before publishing a replacement
  // mode list. Readers only hold m_modeLock long enough to take a snapshot.
  SRWLOCK         m_reloadLock = SRWLOCK_INIT;
  mutable SRWLOCK m_modeLock   = SRWLOCK_INIT;

  CSettings::DisplayModes m_modes;
  CEdid                   m_edid;
  bool                    m_hdrEnabled = false;

  bool LoadModes(const FrameMemoryLimits& limits);
  CSettings::DisplayModes SnapshotModes(bool * hdrEnabled = nullptr) const;

  static bool AlignUp(UINT64 value, UINT64 alignment, UINT64& result);
  static bool CalculateFrameSize(uint32_t width, uint32_t height,
    UINT64& frameSize);
  static bool GetResolutionMemoryRequirements(uint32_t width,
    uint32_t height, UINT64 alignment, const FrameMemoryLimits& limits,
    UINT64& frameSize, UINT64& requiredSize);
  static uint32_t RecommendedMemorySizeMiB(UINT64 requiredSize);

public:
  explicit CDisplayConfiguration(CSettings& settings);

  CDisplayConfiguration(const CDisplayConfiguration&) = delete;
  CDisplayConfiguration& operator=(const CDisplayConfiguration&) = delete;

  bool Load(const FrameMemoryLimits& limits);
  bool ReloadSettings(const FrameMemoryLimits& limits);
  ResolutionResult SetResolution(uint32_t width, uint32_t height,
    const FrameMemoryLimits& limits);

  void InitializeEdid(bool hdr);
  void RebuildEdid(bool hdr);
  Description GetDescription() const;

  NTSTATUS ParseMonitorDescription(
    const IDARG_IN_PARSEMONITORDESCRIPTION * inArgs,
    IDARG_OUT_PARSEMONITORDESCRIPTION * outArgs) const;
  NTSTATUS MonitorGetDefaultModes(
    const IDARG_IN_GETDEFAULTDESCRIPTIONMODES * inArgs,
    IDARG_OUT_GETDEFAULTDESCRIPTIONMODES * outArgs) const;
  NTSTATUS MonitorQueryTargetModes(
    const IDARG_IN_QUERYTARGETMODES * inArgs,
    IDARG_OUT_QUERYTARGETMODES * outArgs) const;

#ifdef HAS_IDDCX_110
  NTSTATUS ParseMonitorDescription2(
    const IDARG_IN_PARSEMONITORDESCRIPTION2 * inArgs,
    IDARG_OUT_PARSEMONITORDESCRIPTION * outArgs) const;
  NTSTATUS MonitorQueryTargetModes2(
    const IDARG_IN_QUERYTARGETMODES2 * inArgs,
    IDARG_OUT_QUERYTARGETMODES * outArgs) const;
#endif
};
