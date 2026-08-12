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

#include "CSRWLock.h"

#include "config/CSettings.h"
#include "display/CEdid.h"
#include "display/IddCxCompat.h"
#include "transport/FrameCaps.h"

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
    UNSUPPORTED,
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
  CSRWLock         m_reloadLock;
  mutable CSRWLock m_modeLock;

  CSettings::DisplayModes m_modes;
  CEdid                   m_edid;
  bool                    m_hdrEnabled = false;

  bool LoadModes(const FrameCaps& caps);
  CSettings::DisplayModes SnapshotModes(bool * hdrEnabled = nullptr) const;

public:
  explicit CDisplayConfiguration(CSettings& settings);

  CDisplayConfiguration(const CDisplayConfiguration&) = delete;
  CDisplayConfiguration& operator=(const CDisplayConfiguration&) = delete;

  bool Load(const FrameCaps& caps);
  bool ReloadSettings(const FrameCaps& caps);
  ResolutionResult SetResolution(uint32_t width, uint32_t height,
    const FrameCaps& caps);

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
