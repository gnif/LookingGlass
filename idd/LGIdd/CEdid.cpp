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

#include "CEdid.h"

#include <algorithm>
#include <string.h>

static const UINT EDID_BLOCK_SIZE = 128;
static const UINT EDID_DTD_SIZE   = 18;

static const UINT EDID_STANDARD_TIMING_COUNT              = 8;
static const UINT EDID_BASE_DESCRIPTOR_COUNT              = 4;
static const UINT EDID_BASE_DETAILED_TIMING_COUNT         = 3;
static const UINT EDID_BASE_MONITOR_NAME_DESCRIPTOR_INDEX = 3;

static const UINT CTA_HEADER_SIZE                 = 4;
static const UINT CTA_DATA_BLOCK_MAX_PAYLOAD_SIZE = 31;

static const BYTE EDID_HEADER[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };

static const WORD EDID_MANUFACTURER_ID_LGD = 0x30e4;
static const WORD EDID_PRODUCT_CODE        = 0x1ddd;
static const BYTE EDID_SERIAL_NUMBER[4]    = { 0x01, 0x00, 0x00, 0x00 };

static const BYTE EDID_MANUFACTURE_WEEK      = 1;
static const BYTE EDID_MANUFACTURE_YEAR_2026 = 36; // 1990 + 36 = 2026
static const BYTE EDID_VERSION               = 1;
static const BYTE EDID_REVISION              = 4;

static const BYTE EDID_VIDEO_INPUT_DIGITAL_8BPC       = 0xa0;
static const BYTE EDID_VIDEO_INPUT_DIGITAL_10BPC      = 0xb0;
static const BYTE EDID_DISPLAY_GAMMA_2_2              = 0x78;
static const BYTE EDID_FEATURES_PREFERRED_TIMING_RGB  = 0x0a;
static const BYTE EDID_FEATURES_PREFERRED_TIMING_SRGB = 0x0e;

static const BYTE EDID_STANDARD_TIMING_UNUSED_X          = 0x01;
static const BYTE EDID_STANDARD_TIMING_UNUSED_AR_REFRESH = 0x01;

static const BYTE EDID_DESCRIPTOR_MONITOR_NAME                  = 0xfc;
static const BYTE EDID_DTD_FLAGS_DIGITAL_SEPARATE_SYNC_POSITIVE = 0x1e;

static const BYTE CTA_EXTENSION_TAG = 0x02;
static const BYTE CTA_REVISION      = 0x03;

static const BYTE CTA_DATA_BLOCK_TAG_EXTENDED = 0x07;
static const BYTE CTA_DATA_BLOCK_LENGTH_MASK  = 0x1f;

static const BYTE CTA_EXTENDED_TAG_COLORIMETRY         = 0x05;
static const BYTE CTA_EXTENDED_TAG_HDR_STATIC_METADATA = 0x06;

static const BYTE CTA_HDR_EOTF_TRADITIONAL_SDR            = (BYTE)(1 << 0);
static const BYTE CTA_HDR_EOTF_SMPTE_ST_2084              = (BYTE)(1 << 2);
static const BYTE CTA_HDR_STATIC_METADATA_TYPE_1          = (BYTE)(1 << 0);
// The virtual display is a transport rather than a physical light-emitting
// device. Advertise the complete PQ range so Windows preserves HDR content
// for the real host display instead of mapping it to an arbitrary virtual
// peak or frame-average limit. The maximum values encode approximately
// 10,000 cd/m^2, while zero leaves the nonexistent physical black level
// unspecified.
static const BYTE CTA_HDR_DESIRED_MAX_LUMINANCE           = 245;
static const BYTE CTA_HDR_DESIRED_MAX_FRAME_AVG_LUMINANCE = 245;
static const BYTE CTA_HDR_DESIRED_MIN_LUMINANCE           = 0;

static const BYTE CTA_COLORIMETRY_BT2020_RGB = (BYTE)(1 << 7);

#pragma pack(push, 1)
struct EdidLe16
{
  BYTE lo;
  BYTE hi;
};

struct EdidBe16
{
  BYTE hi;
  BYTE lo;
};

struct EdidStandardTiming
{
  BYTE horizontalActivePixels;
  BYTE aspectRatioAndRefreshRate;
};

struct EdidDetailedTimingDescriptor
{
  EdidLe16 pixelClock10KHz;

  BYTE hActiveLo;
  BYTE hBlankLo;
  BYTE hActiveBlankHi;

  BYTE vActiveLo;
  BYTE vBlankLo;
  BYTE vActiveBlankHi;

  BYTE hFrontPorchLo;
  BYTE hSyncPulseWidthLo;
  BYTE vFrontPorchSyncPulseWidthLo;
  BYTE syncPorchPulseWidthHi;

  BYTE imageWidthMmLo;
  BYTE imageHeightMmLo;
  BYTE imageSizeMmHi;

  BYTE hBorder;
  BYTE vBorder;
  BYTE flags;
};

struct EdidMonitorNameDescriptor
{
  EdidLe16 pixelClock;
  BYTE reserved0;
  BYTE descriptorTag;
  BYTE reserved1;
  char name[13];
};

union EdidDescriptor
{
  EdidDetailedTimingDescriptor detailedTiming;
  EdidMonitorNameDescriptor monitorName;
  BYTE raw[EDID_DTD_SIZE];
};

struct EdidBaseBlock
{
  BYTE header[8];

  EdidBe16 manufacturerId;
  EdidLe16 productCode;
  BYTE serialNumber[4];

  BYTE manufactureWeek;
  BYTE manufactureYear;
  BYTE version;
  BYTE revision;

  BYTE videoInputDefinition;
  BYTE horizontalSizeCm;
  BYTE verticalSizeCm;
  BYTE displayGamma;
  BYTE supportedFeatures;

  BYTE chromaticityCoordinates[10];
  BYTE establishedTimings[3];
  EdidStandardTiming standardTimings[EDID_STANDARD_TIMING_COUNT];

  EdidDescriptor descriptors[EDID_BASE_DESCRIPTOR_COUNT];

  BYTE extensionBlockCount;
  BYTE checksum;
};

struct CtaDataBlockHeader
{
  BYTE value;
};

struct CtaExtensionBlock
{
  BYTE tag;
  BYTE revision;
  BYTE dtdOffset;
  BYTE flags;
  BYTE payload[EDID_BLOCK_SIZE - CTA_HEADER_SIZE - 1];
  BYTE checksum;
};

struct CtaHdrStaticMetadataDataBlock
{
  CtaDataBlockHeader header;
  BYTE extendedTag;
  BYTE eotf;
  BYTE staticMetadataDescriptor;
  BYTE desiredContentMaxLuminance;
  BYTE desiredContentMaxFrameAverageLuminance;
  BYTE desiredContentMinLuminance;
};

struct CtaColorimetryDataBlock
{
  CtaDataBlockHeader header;
  BYTE extendedTag;
  BYTE colorimetry;
  BYTE metadataAndAdditionalColorimetry;
};

#pragma pack(pop)

static_assert(sizeof(EdidLe16) == 2, "Unexpected EDID little-endian word size");
static_assert(sizeof(EdidBe16) == 2, "Unexpected EDID big-endian word size");
static_assert(sizeof(EdidStandardTiming) == 2, "Unexpected EDID standard timing size");
static_assert(sizeof(EdidDetailedTimingDescriptor) == EDID_DTD_SIZE,
  "Unexpected EDID detailed timing descriptor size");
static_assert(sizeof(EdidMonitorNameDescriptor) == EDID_DTD_SIZE,
  "Unexpected EDID monitor name descriptor size");
static_assert(sizeof(EdidDescriptor) == EDID_DTD_SIZE,
  "Unexpected EDID descriptor size");
static_assert(sizeof(EdidBaseBlock) == EDID_BLOCK_SIZE,
  "Unexpected EDID base block size");

static_assert(sizeof(CtaExtensionBlock) == EDID_BLOCK_SIZE,
  "Unexpected CTA extension block size");
static_assert(sizeof(CtaHdrStaticMetadataDataBlock) == 7,
  "Unexpected HDR static metadata data block size");
static_assert(sizeof(CtaColorimetryDataBlock) == 4,
  "Unexpected colorimetry data block size");
static_assert(CTA_HEADER_SIZE +
  sizeof(CtaHdrStaticMetadataDataBlock) +
  sizeof(CtaColorimetryDataBlock) <= EDID_BLOCK_SIZE - 1,
  "CTA data blocks exceed extension block space");

static void SetBe16(EdidBe16& dst, DWORD value)
{
  dst.hi = (BYTE)((value >> 8) & 0xff);
  dst.lo = (BYTE)(value & 0xff);
}

static void SetLe16(EdidLe16& dst, DWORD value)
{
  dst.lo = (BYTE)(value & 0xff);
  dst.hi = (BYTE)((value >> 8) & 0xff);
}

static BYTE Lo8(DWORD value)
{
  return (BYTE)(value & 0xff);
}

static BYTE PackMsbNibbles(DWORD upperValue, DWORD lowerValue)
{
  return (BYTE)((((upperValue >> 8) & 0x0f) << 4) |
    ((lowerValue >> 8) & 0x0f));
}

static BYTE PackLowNibbles(DWORD upperValue, DWORD lowerValue)
{
  return (BYTE)(((upperValue & 0x0f) << 4) |
    (lowerValue & 0x0f));
}

static BYTE PackSyncPorchPulseWidthHi(
  DWORD hFrontPorch,
  DWORD hSyncPulseWidth,
  DWORD vFrontPorch,
  DWORD vSyncPulseWidth)
{
  return (BYTE)(
    (((hFrontPorch     >> 8) & 0x03) << 6) |
    (((hSyncPulseWidth >> 8) & 0x03) << 4) |
    (((vFrontPorch     >> 4) & 0x03) << 2) |
    ((vSyncPulseWidth  >> 4) & 0x03));
}

static EdidStandardTiming MakeUnusedStandardTiming()
{
  EdidStandardTiming timing = {};
  timing.horizontalActivePixels    = EDID_STANDARD_TIMING_UNUSED_X;
  timing.aspectRatioAndRefreshRate = EDID_STANDARD_TIMING_UNUSED_AR_REFRESH;
  return timing;
}

static WORD EdidChromaticity(double value)
{
  return (WORD)min(1023.0, max(0.0, value * 1024.0 + 0.5));
}

static void SetChromaticityCoordinates(BYTE coordinates[10], bool hdr)
{
  // Describe the wire gamut. Accelerated HDR uses the BT.2020 container;
  // software rendering is SDR-only and uses the standard sRGB/BT.709 gamut.
  const WORD rx = EdidChromaticity(hdr ? 0.7080 : 0.6400);
  const WORD ry = EdidChromaticity(hdr ? 0.2920 : 0.3300);
  const WORD gx = EdidChromaticity(hdr ? 0.1700 : 0.3000);
  const WORD gy = EdidChromaticity(hdr ? 0.7970 : 0.6000);
  const WORD bx = EdidChromaticity(hdr ? 0.1310 : 0.1500);
  const WORD by = EdidChromaticity(hdr ? 0.0460 : 0.0600);
  const WORD wx = EdidChromaticity(0.3127);
  const WORD wy = EdidChromaticity(0.3290);

  coordinates[0] = (BYTE)(((rx & 3) << 6) | ((ry & 3) << 4) |
      ((gx & 3) << 2) | (gy & 3));
  coordinates[1] = (BYTE)(((bx & 3) << 6) | ((by & 3) << 4) |
      ((wx & 3) << 2) | (wy & 3));
  coordinates[2] = (BYTE)(rx >> 2);
  coordinates[3] = (BYTE)(ry >> 2);
  coordinates[4] = (BYTE)(gx >> 2);
  coordinates[5] = (BYTE)(gy >> 2);
  coordinates[6] = (BYTE)(bx >> 2);
  coordinates[7] = (BYTE)(by >> 2);
  coordinates[8] = (BYTE)(wx >> 2);
  coordinates[9] = (BYTE)(wy >> 2);
}

static BYTE GetVideoInputDefinition(bool hdr)
{
  return hdr ?
    EDID_VIDEO_INPUT_DIGITAL_10BPC :
    EDID_VIDEO_INPUT_DIGITAL_8BPC;
}

static void InitEdidBaseBlock(EdidBaseBlock& base, bool hdr)
{
  memcpy(base.header, EDID_HEADER, sizeof(base.header));

  // Manufacturer ID: LGD, product/serial values identify the virtual monitor.
  SetBe16(base.manufacturerId, EDID_MANUFACTURER_ID_LGD);
  SetLe16(base.productCode   , EDID_PRODUCT_CODE);
  memcpy (base.serialNumber  , EDID_SERIAL_NUMBER, sizeof(base.serialNumber));

  base.manufactureWeek = EDID_MANUFACTURE_WEEK;
  base.manufactureYear = EDID_MANUFACTURE_YEAR_2026;
  base.version         = EDID_VERSION;
  base.revision        = EDID_REVISION;

  base.videoInputDefinition = GetVideoInputDefinition(hdr);
  // This is a transport endpoint rather than a physical panel, so leave its
  // physical dimensions unspecified instead of imposing a false DPI/aspect.
  base.horizontalSizeCm  = 0;
  base.verticalSizeCm    = 0;
  base.displayGamma      = EDID_DISPLAY_GAMMA_2_2;
  base.supportedFeatures = hdr ?
    EDID_FEATURES_PREFERRED_TIMING_RGB :
    EDID_FEATURES_PREFERRED_TIMING_SRGB;
  SetChromaticityCoordinates(base.chromaticityCoordinates, hdr);

  for (UINT i = 0; i < EDID_STANDARD_TIMING_COUNT; ++i)
    base.standardTimings[i] = MakeUnusedStandardTiming();

  base.extensionBlockCount = 1;
}

bool CEdid::GetTiming(Timing& timing, const CSettings::DisplayMode& mode)
{
  timing = {};

  timing.hActive = mode.width;
  timing.vActive = mode.height;

  if (timing.hActive == 0 || timing.vActive == 0 || mode.refresh == 0)
    return false;

  timing.hBlank = std::max<DWORD>(160,
    ((timing.hActive / 20) + 7) & ~7UL);
  timing.vBlank = std::max<DWORD>(30, timing.vActive / 20);

  timing.hSync = std::max<DWORD>(32, timing.hActive / 100);
  timing.hSync = (timing.hSync + 7) & ~7UL;

  timing.hFront = std::max<DWORD>(48, timing.hBlank / 3);
  timing.hFront = (timing.hFront + 7) & ~7UL;

  if (timing.hFront + timing.hSync >= timing.hBlank)
  {
    timing.hFront = 48;
    timing.hSync  = 32;
  }

  timing.vFront = 3;
  timing.vSync  = 5;
  if (timing.vFront + timing.vSync >= timing.vBlank)
    return false;

  const UINT64 pixelClock =
    (UINT64)(timing.hActive + timing.hBlank) *
    (UINT64)(timing.vActive + timing.vBlank) *
    (UINT64)mode.refresh;
  const UINT64 pixelClock10KHz = (pixelClock + 5000) / 10000;
  timing.pixelClock = pixelClock10KHz * 10000;
  return timing.pixelClock != 0;
}

static bool MakeDetailedTiming(
  EdidDetailedTimingDescriptor& descriptor,
  const CSettings::DisplayMode& mode)
{
  memset(&descriptor, 0, sizeof(descriptor));

  CEdid::Timing timing;
  if (!CEdid::GetTiming(timing, mode) ||
      timing.hActive > 4095 || timing.vActive > 4095 ||
      timing.hBlank  > 4095 || timing.vBlank  > 4095)
    return false;

  const UINT64 pixelClock10KHz = timing.pixelClock / 10000;
  if (pixelClock10KHz == 0 || pixelClock10KHz > 0xffff)
    return false;

  SetLe16(descriptor.pixelClock10KHz, (DWORD)pixelClock10KHz);

  descriptor.hActiveLo      = Lo8(timing.hActive);
  descriptor.hBlankLo       = Lo8(timing.hBlank);
  descriptor.hActiveBlankHi = PackMsbNibbles(timing.hActive, timing.hBlank);

  descriptor.vActiveLo      = Lo8(timing.vActive);
  descriptor.vBlankLo       = Lo8(timing.vBlank);
  descriptor.vActiveBlankHi = PackMsbNibbles(timing.vActive, timing.vBlank);

  descriptor.hFrontPorchLo     = Lo8(timing.hFront);
  descriptor.hSyncPulseWidthLo = Lo8(timing.hSync);
  descriptor.vFrontPorchSyncPulseWidthLo =
    PackLowNibbles(timing.vFront, timing.vSync);
  descriptor.syncPorchPulseWidthHi = PackSyncPorchPulseWidthHi(
    timing.hFront, timing.hSync, timing.vFront, timing.vSync);

  descriptor.imageWidthMmLo  = 0;
  descriptor.imageHeightMmLo = 0;
  descriptor.imageSizeMmHi   = 0;

  descriptor.flags = EDID_DTD_FLAGS_DIGITAL_SEPARATE_SYNC_POSITIVE;
  return true;
}

static void MakeMonitorName(
  EdidMonitorNameDescriptor& monitorName,
  const char* name)
{
  memset(&monitorName, 0, sizeof(monitorName));

  monitorName.descriptorTag = EDID_DESCRIPTOR_MONITOR_NAME;

  UINT len = 0;
  for (; len < sizeof(monitorName.name) && name[len]; ++len)
    monitorName.name[len] = name[len];

  if (len < sizeof(monitorName.name))
    monitorName.name[len++] = '\n';

  for (; len < sizeof(monitorName.name); ++len)
    monitorName.name[len] = ' ';
}

static CtaDataBlockHeader MakeCtaDataBlockHeader(BYTE tag, UINT payloadSize)
{
  CtaDataBlockHeader header = {};

  if (payloadSize > CTA_DATA_BLOCK_MAX_PAYLOAD_SIZE)
    payloadSize = CTA_DATA_BLOCK_MAX_PAYLOAD_SIZE;

  header.value = (BYTE)((tag << 5) |
    (payloadSize & CTA_DATA_BLOCK_LENGTH_MASK));

  return header;
}

template <typename T>
static void AppendCtaDataBlock(BYTE* cta, UINT& offset, const T& block)
{
  static_assert(sizeof(T) > sizeof(CtaDataBlockHeader),
    "CTA data block has no payload");
  static_assert(sizeof(T) - sizeof(CtaDataBlockHeader) <=
    CTA_DATA_BLOCK_MAX_PAYLOAD_SIZE,
    "CTA data block payload exceeds header length field");

  memcpy(cta + offset, &block, sizeof(block));
  offset += (UINT)sizeof(block);
}

static CtaHdrStaticMetadataDataBlock MakeCtaHdrStaticMetadataDataBlock()
{
  CtaHdrStaticMetadataDataBlock block = {};

  block.header = MakeCtaDataBlockHeader(CTA_DATA_BLOCK_TAG_EXTENDED,
    (UINT)(sizeof(block) - sizeof(block.header)));

  block.extendedTag = CTA_EXTENDED_TAG_HDR_STATIC_METADATA;
  block.eotf        = (BYTE)(
    CTA_HDR_EOTF_TRADITIONAL_SDR |
    CTA_HDR_EOTF_SMPTE_ST_2084);

  block.staticMetadataDescriptor               = CTA_HDR_STATIC_METADATA_TYPE_1;
  block.desiredContentMaxLuminance             = CTA_HDR_DESIRED_MAX_LUMINANCE;
  block.desiredContentMaxFrameAverageLuminance = CTA_HDR_DESIRED_MAX_FRAME_AVG_LUMINANCE;
  block.desiredContentMinLuminance             = CTA_HDR_DESIRED_MIN_LUMINANCE;
  return block;
}

static CtaColorimetryDataBlock MakeCtaColorimetryDataBlock()
{
  CtaColorimetryDataBlock block = {};

  block.header = MakeCtaDataBlockHeader(CTA_DATA_BLOCK_TAG_EXTENDED,
    (UINT)(sizeof(block) - sizeof(block.header)));
  block.extendedTag                      = CTA_EXTENDED_TAG_COLORIMETRY;
  block.colorimetry                      = CTA_COLORIMETRY_BT2020_RGB;
  block.metadataAndAdditionalColorimetry = 0;
  return block;
}

void CEdid::SetChecksum(BYTE* block)
{
  BYTE sum = 0;
  for (UINT i = 0; i < EDID_BLOCK_SIZE - 1; ++i)
    sum = (BYTE)(sum + block[i]);

  block[EDID_BLOCK_SIZE - 1] = (BYTE)(0 - sum);
}

void CEdid::WriteMonitorName(BYTE* desc, const char* name)
{
  EdidMonitorNameDescriptor monitorName = {};
  MakeMonitorName(monitorName, name);
  memcpy(desc, &monitorName, sizeof(monitorName));
}

bool CEdid::WriteDetailedTiming(BYTE* dtd, const CSettings::DisplayMode& mode)
{
  EdidDetailedTimingDescriptor timing = {};

  if (!MakeDetailedTiming(timing, mode))
    return false;

  memcpy(dtd, &timing, sizeof(timing));
  return true;
}

void CEdid::Build(const CSettings::DisplayModes& modes, bool hdr)
{
  m_data.assign(
    static_cast<std::vector<BYTE, std::allocator<BYTE>>::size_type>(
      EDID_BLOCK_SIZE) * 2,
    0);

  EdidBaseBlock baseBlock = {};
  InitEdidBaseBlock(baseBlock, hdr);

  CSettings::DisplayModes sorted = modes;
  std::stable_sort(sorted.begin(), sorted.end(),
    [](const CSettings::DisplayMode& a, const CSettings::DisplayMode& b)
    {
      if (a.preferred != b.preferred)
        return a.preferred && !b.preferred;
      if (a.width != b.width)
        return a.width > b.width;
      if (a.height != b.height)
        return a.height > b.height;
      return a.refresh > b.refresh;
    });

  UINT modeIndex    = 0;
  UINT baseDtdIndex = 0;

  for (; modeIndex < sorted.size() &&
    baseDtdIndex < EDID_BASE_DETAILED_TIMING_COUNT;
    ++modeIndex)
  {
    // never include the extra mode in the EDID
    if (sorted[modeIndex].extraMode)
      continue;

    if (MakeDetailedTiming(
      baseBlock.descriptors[baseDtdIndex].detailedTiming,
      sorted[modeIndex]))
    {
      ++baseDtdIndex;
    }
  }

  MakeMonitorName(
    baseBlock.descriptors[EDID_BASE_MONITOR_NAME_DESCRIPTOR_INDEX].monitorName,
    "Looking Glass");

  SetChecksum(reinterpret_cast<BYTE*>(&baseBlock));
  memcpy(m_data.data(), &baseBlock, sizeof(baseBlock));

  CtaExtensionBlock ctaBlock = {};
  BYTE* cta = reinterpret_cast<BYTE*>(&ctaBlock);

  ctaBlock.tag      = CTA_EXTENSION_TAG;
  ctaBlock.revision = CTA_REVISION;

  UINT dataOffset = CTA_HEADER_SIZE;
  if (hdr)
  {
    AppendCtaDataBlock(cta, dataOffset, MakeCtaHdrStaticMetadataDataBlock      ());
    AppendCtaDataBlock(cta, dataOffset, MakeCtaColorimetryDataBlock            ());
  }

  ctaBlock.dtdOffset = (BYTE)dataOffset;
  ctaBlock.flags     = 0x00;

  UINT ctaDtdWrite = dataOffset;
  for (; modeIndex < sorted.size() &&
    ctaDtdWrite + EDID_DTD_SIZE <= EDID_BLOCK_SIZE - 1;
    ++modeIndex)
  {
    // never include the extra mode in the EDID
    if (sorted[modeIndex].extraMode)
      continue;

    if (WriteDetailedTiming(cta + ctaDtdWrite, sorted[modeIndex]))
      ctaDtdWrite += EDID_DTD_SIZE;
  }

  SetChecksum(cta);
  memcpy(m_data.data() + sizeof(baseBlock), &ctaBlock, sizeof(ctaBlock));
}
