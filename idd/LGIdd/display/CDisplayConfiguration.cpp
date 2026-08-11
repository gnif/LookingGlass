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

#include "display/CDisplayConfiguration.h"

#include "CDebug.h"
#include "CSRWLock.h"

#include <d3d12.h>
#include <iterator>
#include <utility>

static const UINT64 FRAME_BYTES_PER_PIXEL = 4;

bool CDisplayConfiguration::AlignUp(
  UINT64 value, UINT64 alignment, UINT64& result)
{
  if (!alignment || (alignment & (alignment - 1)))
    return false;

  const UINT64 mask = alignment - 1;
  result = (value + mask) & ~mask;
  return true;
}

bool CDisplayConfiguration::CalculateFrameSize(
  uint32_t width, uint32_t height, UINT64& frameSize)
{
  frameSize = 0;
  if (!width || !height)
    return false;

  UINT64 pitch;
  if (!AlignUp((UINT64)width * FRAME_BYTES_PER_PIXEL,
      D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, pitch))
    return false;

  frameSize = pitch * height;
  return true;
}

bool CDisplayConfiguration::GetResolutionMemoryRequirements(
  uint32_t width, uint32_t height, UINT64 alignment,
  const FrameMemoryLimits& limits, UINT64& frameSize, UINT64& requiredSize)
{
  frameSize    = 0;
  requiredSize = 0;

  if (!alignment || !limits.frameMemoryOffset || !limits.bufferCount ||
      !CalculateFrameSize(width, height, frameSize))
    return false;

  UINT64 frameAllocationSize;
  if (!AlignUp(frameSize + alignment, alignment, frameAllocationSize))
    return false;

  UINT64 frameMemoryStart;
  if (!AlignUp(limits.frameMemoryOffset, alignment, frameMemoryStart))
    return false;

  requiredSize = frameMemoryStart +
    frameAllocationSize * limits.bufferCount;
  return true;
}

uint32_t CDisplayConfiguration::RecommendedMemorySizeMiB(
  UINT64 requiredSize)
{
  UINT64 sizeMiB = requiredSize / 1048576;
  if (requiredSize % 1048576)
    ++sizeMiB;

  UINT64 result = 1;
  while (result < sizeMiB && result <= UINT32_MAX / 2)
    result <<= 1;

  return result < sizeMiB ? UINT32_MAX : (uint32_t)result;
}

#ifdef HAS_IDDCX_110
static inline IDDCX_WIRE_BITS_PER_COMPONENT GetWireBitsPerComponent(bool hdr)
{
  IDDCX_WIRE_BITS_PER_COMPONENT bits = {};
  // This describes the virtual monitor wire, not the swap-chain format.
  // HDR uses a 10-bpc PQ wire while CAN_PROCESS_FP16 requests the FP16/scRGB
  // source surface that Looking Glass converts for transport.
  bits.Rgb = IDDCX_BITS_PER_COMPONENT_8;
  if (hdr)
    bits.Rgb = (IDDCX_BITS_PER_COMPONENT)(bits.Rgb |
      IDDCX_BITS_PER_COMPONENT_10);
  bits.YCbCr444 = IDDCX_BITS_PER_COMPONENT_NONE;
  bits.YCbCr422 = IDDCX_BITS_PER_COMPONENT_NONE;
  bits.YCbCr420 = IDDCX_BITS_PER_COMPONENT_NONE;
  return bits;
}
#endif

CDisplayConfiguration::CDisplayConfiguration(CSettings& settings) :
  m_settings(settings)
{
}

bool CDisplayConfiguration::LoadModes(const FrameMemoryLimits& limits)
{
  const CSettings::DisplayModes configuredModes =
    m_settings.LoadModes();

  // Build the new mode list into a local first so readers never observe a
  // reallocation of the live container. Publishing it is a pointer swap.
  CSettings::DisplayModes newModes;
  newModes.reserve(configuredModes.size());

  const UINT64 alignment = limits.alignment ? limits.alignment :
    D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;

  bool hasPreferred = false;
  for (const auto& configuredMode : configuredModes)
  {
    UINT64 frameSize;
    UINT64 requiredMemorySize;
    if (!GetResolutionMemoryRequirements(configuredMode.width,
        configuredMode.height, alignment, limits, frameSize,
        requiredMemorySize))
    {
      DEBUG_WARN("Filtering invalid %s mode %ux%u@%.3f",
        configuredMode.extraMode ? "extra" : "configured",
        configuredMode.width, configuredMode.height,
        configuredMode.refreshMilliHz / 1000.0);
      continue;
    }

    if (requiredMemorySize > limits.capacity)
    {
      DEBUG_WARN(
        "Filtering %s mode %ux%u@%.3f: requires %llu bytes of transport memory, only %llu bytes are available",
        configuredMode.extraMode ? "extra" : "configured",
        configuredMode.width, configuredMode.height,
        configuredMode.refreshMilliHz / 1000.0,
        (unsigned long long)requiredMemorySize,
        (unsigned long long)limits.capacity);
      continue;
    }

    CSettings::DisplayMode mode = configuredMode;
    if (mode.preferred)
    {
      mode.preferred = !hasPreferred;
      hasPreferred   = true;
    }
    newModes.push_back(mode);
  }

  if (newModes.empty())
  {
    DEBUG_ERROR("No configured display modes fit in transport memory");
    return false;
  }

  // ExtraMode may have been the preferred mode. If it did not fit, promote
  // the first remaining mode so the list still has a valid preference.
  if (!hasPreferred)
    newModes.front().preferred = true;

  CSRWExclusiveLock lock(m_modeLock);
  m_modes = std::move(newModes);
  return true;
}

bool CDisplayConfiguration::Load(const FrameMemoryLimits& limits)
{
  return LoadModes(limits);
}

bool CDisplayConfiguration::ReloadSettings(
  const FrameMemoryLimits& limits)
{
  bool modesLoaded = false;
  {
    CSRWExclusiveLock reloadLock(m_reloadLock);

    bool settingsUpdated = true;
    CSettings::DisplayMode extraMode = {};
    if (m_settings.GetExtraMode(extraMode))
    {
      const unsigned refreshMilliHz =
        m_settings.GetDefaultRefreshMilliHz();
      if (extraMode.refreshMilliHz != refreshMilliHz)
      {
        extraMode.refreshMilliHz = refreshMilliHz;
        settingsUpdated = m_settings.SetExtraMode(extraMode);
      }
    }

    if (settingsUpdated)
      modesLoaded = LoadModes(limits);
  }

  if (!modesLoaded)
    DEBUG_ERROR("Failed to reload the display mode list");
  return modesLoaded;
}

CDisplayConfiguration::ResolutionResult
CDisplayConfiguration::SetResolution(
  uint32_t width, uint32_t height, const FrameMemoryLimits& limits)
{
  ResolutionResult result;

  UINT64 frameSize;
  UINT64 requiredMemorySize;
  if (!GetResolutionMemoryRequirements(width, height, limits.alignment,
      limits, frameSize, requiredMemorySize))
  {
    DEBUG_WARN("Ignoring invalid resolution request: %ux%u", width, height);
    return result;
  }

  if (requiredMemorySize > limits.capacity)
  {
    result.status      = ResolutionStatus::TOO_LARGE;
    result.requiredMiB = RecommendedMemorySizeMiB(requiredMemorySize);
    DEBUG_WARN(
      "Refusing resolution %ux%u: frame requires %llu bytes, only %llu bytes are available; transport memory must be at least %u MiB",
      width, height,
      (unsigned long long)frameSize,
      (unsigned long long)limits.maxFrameSize,
      result.requiredMiB);
    return result;
  }

  CSettings::DisplayMode mode = {};
  mode.width          = width;
  mode.height         = height;
  mode.refreshMilliHz = m_settings.GetDefaultRefreshMilliHz();
  mode.preferred      = true;

  {
    CSRWExclusiveLock reloadLock(m_reloadLock);
    if (!m_settings.SetExtraMode(mode))
      result.status = ResolutionStatus::SETTINGS_FAILED;
    else if (!LoadModes(limits))
      result.status = ResolutionStatus::MODES_FAILED;
    else
    {
      result.status = ResolutionStatus::SUCCESS;
      result.mode   = mode;
    }
  }

  if (result.status != ResolutionStatus::SUCCESS)
    DEBUG_ERROR("Failed to rebuild the display mode list");
  return result;
}

void CDisplayConfiguration::InitializeEdid(bool hdr)
{
  CSRWExclusiveLock lock(m_modeLock);
  if (m_edid.Size())
    return;

  m_edid.Build(hdr);
  m_hdrEnabled = hdr;
}

void CDisplayConfiguration::RebuildEdid(bool hdr)
{
  CSRWExclusiveLock lock(m_modeLock);
  m_edid.Build(hdr);
  m_hdrEnabled = hdr;
}

CDisplayConfiguration::Description
CDisplayConfiguration::GetDescription() const
{
  Description result;
  CSRWSharedLock lock(m_modeLock);
  result.modeCount = m_modes.size();
  if (m_edid.Size())
    result.edid.assign(m_edid.Data(), m_edid.Data() + m_edid.Size());
  return result;
}

CSettings::DisplayModes CDisplayConfiguration::SnapshotModes(
  bool * hdrEnabled) const
{
  CSRWSharedLock lock(m_modeLock);
  if (hdrEnabled)
    *hdrEnabled = m_hdrEnabled;
  return m_modes;
}

static UINT64 GreatestCommonDivisor(UINT64 a, UINT64 b)
{
  while (b)
  {
    const UINT64 remainder = a % b;
    a = b;
    b = remainder;
  }
  return a;
}

static void SetSignalRate(DISPLAYCONFIG_RATIONAL& rate,
  UINT64 numerator, UINT32 denominator)
{
  const UINT64 divisor = GreatestCommonDivisor(numerator, denominator);
  numerator   /= divisor;
  denominator /= (UINT32)divisor;

  if (numerator <= UINT32_MAX)
  {
    rate.Numerator   = (UINT32)numerator;
    rate.Denominator = denominator;
    return;
  }

  rate.Numerator =
    (UINT32)((numerator + denominator / 2) / denominator);
  rate.Denominator = 1;
}

static inline void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& signal,
  const CSettings::DisplayMode& mode, bool monitorMode)
{
  CEdid::Timing timing;
  if (!CEdid::GetTiming(timing, mode))
    return;

  signal.activeSize.cx = timing.hActive;
  signal.activeSize.cy = timing.vActive;
  signal.totalSize.cx  = timing.hActive + timing.hBlank;
  signal.totalSize.cy  = timing.vActive + timing.vBlank;

  signal.AdditionalSignalInfo.vSyncFreqDivider = monitorMode ? 0 : 1;
  signal.AdditionalSignalInfo.videoStandard    = 255;

  SetSignalRate(signal.vSyncFreq, mode.refreshMilliHz, 1000);
  SetSignalRate(signal.hSyncFreq,
    (UINT64)mode.refreshMilliHz * signal.totalSize.cy, 1000);

  signal.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
  signal.pixelRate        = timing.pixelClock;
}

NTSTATUS CDisplayConfiguration::ParseMonitorDescription(
  const IDARG_IN_PARSEMONITORDESCRIPTION * inArgs,
  IDARG_OUT_PARSEMONITORDESCRIPTION * outArgs) const
{
  const CSettings::DisplayModes modes = SnapshotModes();

  outArgs->MonitorModeBufferOutputCount = (UINT)modes.size();
  outArgs->PreferredMonitorModeIdx = 0;
  if (inArgs->MonitorModeBufferInputCount < (UINT)modes.size())
    return inArgs->MonitorModeBufferInputCount > 0 ?
      STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;

  auto * mode = inArgs->pMonitorModes;
  for (auto it = modes.cbegin(); it != modes.cend(); ++it, ++mode)
  {
    mode->Size   = sizeof(IDDCX_MONITOR_MODE);
    mode->Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
    FillSignalInfo(mode->MonitorVideoSignalInfo, *it, true);

    if (it->preferred)
      outArgs->PreferredMonitorModeIdx =
        (UINT)std::distance(modes.cbegin(), it);
  }

  return STATUS_SUCCESS;
}

NTSTATUS CDisplayConfiguration::MonitorGetDefaultModes(
  const IDARG_IN_GETDEFAULTDESCRIPTIONMODES * inArgs,
  IDARG_OUT_GETDEFAULTDESCRIPTIONMODES * outArgs) const
{
  const CSettings::DisplayModes modes = SnapshotModes();

  outArgs->DefaultMonitorModeBufferOutputCount = (UINT)modes.size();
  outArgs->PreferredMonitorModeIdx = 0;
  if (inArgs->DefaultMonitorModeBufferInputCount < (UINT)modes.size())
    return inArgs->DefaultMonitorModeBufferInputCount > 0 ?
      STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;

  auto * mode = inArgs->pDefaultMonitorModes;
  for (auto it = modes.cbegin(); it != modes.cend(); ++it, ++mode)
  {
    mode->Size   = sizeof(IDDCX_MONITOR_MODE);
    mode->Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER;
    FillSignalInfo(mode->MonitorVideoSignalInfo, *it, true);

    if (it->preferred)
      outArgs->PreferredMonitorModeIdx =
        (UINT)std::distance(modes.cbegin(), it);
  }

  return STATUS_SUCCESS;
}

NTSTATUS CDisplayConfiguration::MonitorQueryTargetModes(
  const IDARG_IN_QUERYTARGETMODES * inArgs,
  IDARG_OUT_QUERYTARGETMODES * outArgs) const
{
  const CSettings::DisplayModes modes = SnapshotModes();

  outArgs->TargetModeBufferOutputCount = (UINT)modes.size();
  if (inArgs->TargetModeBufferInputCount < (UINT)modes.size())
    return inArgs->TargetModeBufferInputCount > 0 ?
      STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;

  auto * mode = inArgs->pTargetModes;
  for (auto it = modes.cbegin(); it != modes.cend(); ++it, ++mode)
  {
    mode->Size = sizeof(IDDCX_TARGET_MODE);
    FillSignalInfo(
      mode->TargetVideoSignalInfo.targetVideoSignalInfo, *it, false);
  }

  return STATUS_SUCCESS;
}

#ifdef HAS_IDDCX_110
NTSTATUS CDisplayConfiguration::ParseMonitorDescription2(
  const IDARG_IN_PARSEMONITORDESCRIPTION2 * inArgs,
  IDARG_OUT_PARSEMONITORDESCRIPTION * outArgs) const
{
  bool hdrEnabled = false;
  const CSettings::DisplayModes modes = SnapshotModes(&hdrEnabled);

  outArgs->MonitorModeBufferOutputCount = (UINT)modes.size();
  outArgs->PreferredMonitorModeIdx = 0;
  if (inArgs->MonitorModeBufferInputCount < (UINT)modes.size())
    return inArgs->MonitorModeBufferInputCount > 0 ?
      STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;

  auto * mode = inArgs->pMonitorModes;
  for (auto it = modes.cbegin(); it != modes.cend(); ++it, ++mode)
  {
    ZeroMemory(mode, sizeof(*mode));
    mode->Size             = sizeof(IDDCX_MONITOR_MODE2);
    mode->Origin           = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
    FillSignalInfo(mode->MonitorVideoSignalInfo, *it, true);
    mode->BitsPerComponent = GetWireBitsPerComponent(hdrEnabled);

    if (it->preferred)
      outArgs->PreferredMonitorModeIdx =
        (UINT)std::distance(modes.cbegin(), it);
  }

  return STATUS_SUCCESS;
}

NTSTATUS CDisplayConfiguration::MonitorQueryTargetModes2(
  const IDARG_IN_QUERYTARGETMODES2 * inArgs,
  IDARG_OUT_QUERYTARGETMODES * outArgs) const
{
  bool hdrEnabled = false;
  const CSettings::DisplayModes modes = SnapshotModes(&hdrEnabled);

  outArgs->TargetModeBufferOutputCount = (UINT)modes.size();
  if (inArgs->TargetModeBufferInputCount < (UINT)modes.size())
    return STATUS_SUCCESS;

  if (!inArgs->pTargetModes)
    return STATUS_INVALID_PARAMETER;

  auto * mode = inArgs->pTargetModes;
  for (auto it = modes.cbegin(); it != modes.cend(); ++it, ++mode)
  {
    ZeroMemory(mode, sizeof(*mode));
    mode->Size = sizeof(IDDCX_TARGET_MODE2);
    FillSignalInfo(
      mode->TargetVideoSignalInfo.targetVideoSignalInfo, *it, false);
    mode->BitsPerComponent = GetWireBitsPerComponent(hdrEnabled);
  }

  return STATUS_SUCCESS;
}
#endif
