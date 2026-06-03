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

static const WORD EDID_MANUFACTURER_ID_LGD = 0xe430;
static const WORD EDID_PRODUCT_CODE        = 0x1ddd;
static const BYTE EDID_SERIAL_NUMBER[4]    = { 0x01, 0x00, 0x00, 0x00 };

static const BYTE EDID_MANUFACTURE_WEEK      = 1;
static const BYTE EDID_MANUFACTURE_YEAR_2026 = 36; // 1990 + 36 = 2026
static const BYTE EDID_VERSION               = 1;
static const BYTE EDID_REVISION              = 4;

static const BYTE EDID_VIDEO_INPUT_DIGITAL_10BPC_HDMI_A = 0xb2;
static const BYTE EDID_HORIZONTAL_SIZE_CM               = 52;
static const BYTE EDID_VERTICAL_SIZE_CM                 = 29;
static const BYTE EDID_DISPLAY_GAMMA_2_2                = 0x78;
static const BYTE EDID_FEATURES_PREFERRED_TIMING_RGB    = 0x0a;

static const BYTE EDID_STANDARD_TIMING_UNUSED_X          = 0x01;
static const BYTE EDID_STANDARD_TIMING_UNUSED_AR_REFRESH = 0x01;

static const DWORD EDID_IMAGE_WIDTH_MM  = 520;
static const DWORD EDID_IMAGE_HEIGHT_MM = 290;

static const BYTE EDID_DESCRIPTOR_MONITOR_NAME                  = 0xfc;
static const BYTE EDID_DTD_FLAGS_DIGITAL_SEPARATE_SYNC_POSITIVE = 0x1e;

static const BYTE CTA_EXTENSION_TAG = 0x02;
static const BYTE CTA_REVISION      = 0x03;

static const BYTE CTA_DATA_BLOCK_TAG_VENDOR_SPECIFIC = 0x03;
static const BYTE CTA_DATA_BLOCK_TAG_EXTENDED        = 0x07;
static const BYTE CTA_DATA_BLOCK_LENGTH_MASK         = 0x1f;

static const BYTE CTA_EXTENDED_TAG_COLORIMETRY         = 0x05;
static const BYTE CTA_EXTENDED_TAG_HDR_STATIC_METADATA = 0x06;

static const BYTE CTA_HDR_EOTF_TRADITIONAL_SDR            = (BYTE)(1 << 0);
static const BYTE CTA_HDR_EOTF_SMPTE_ST_2084              = (BYTE)(1 << 2);
static const BYTE CTA_HDR_EOTF_HLG                        = (BYTE)(1 << 3);
static const BYTE CTA_HDR_STATIC_METADATA_TYPE_1          = (BYTE)(1 << 0);
static const BYTE CTA_HDR_DESIRED_MAX_LUMINANCE           = 138;
static const BYTE CTA_HDR_DESIRED_MAX_FRAME_AVG_LUMINANCE = 115;
static const BYTE CTA_HDR_DESIRED_MIN_LUMINANCE           = 14;

static const BYTE CTA_COLORIMETRY_OPYCC_601  = (BYTE)(1 << 3);
static const BYTE CTA_COLORIMETRY_OPRGB      = (BYTE)(1 << 4);
static const BYTE CTA_COLORIMETRY_BT2020_YCC = (BYTE)(1 << 6);
static const BYTE CTA_COLORIMETRY_BT2020_RGB = (BYTE)(1 << 7);
static const BYTE CTA_COLORIMETRY_DCI_P3     = (BYTE)(1 << 7);

static const BYTE CTA_HDMI_FORUM_OUI    [3] = { 0xd8, 0x5d, 0xc4 };
static const BYTE CTA_HDMI_LICENSING_OUI[3] = { 0x03, 0x0c, 0x00 };

static const BYTE CTA_HDMI_FORUM_VERSION_1               = 0x01;
static const BYTE CTA_HDMI_FORUM_MAX_TMDS_CHARACTER_RATE = 0x6e;
static const BYTE CTA_HDMI_FORUM_SCDC_PRESENT            = 0x80;

static const BYTE CTA_HDMI_PHYSICAL_ADDRESS_A_B       = 0x00;
static const BYTE CTA_HDMI_PHYSICAL_ADDRESS_C_D       = 0x00;
static const BYTE CTA_HDMI_DEEP_COLOR_30_36           = 0x30;
static const BYTE CTA_HDMI_MAX_TMDS_CLOCK_UNSPECIFIED = 0x00;
static const BYTE CTA_HDMI_LATENCY_FIELDS_NONE        = 0x0b;

#pragma pack(push, 1)
struct EdidLe16
{
  BYTE lo;
  BYTE hi;
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

  EdidLe16 manufacturerId;
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

struct CtaHdmiForumVendorSpecificDataBlock
{
  CtaDataBlockHeader header;
  BYTE ieeeOui[3];
  BYTE version;
  BYTE maxTmdsCharacterRate;
  BYTE flags;
  BYTE reserved;
};

struct CtaHdmiVendorSpecificDataBlock
{
  CtaDataBlockHeader header;
  BYTE ieeeOui[3];
  BYTE physicalAddressAB;
  BYTE physicalAddressCD;
  BYTE flags;
  BYTE maxTmdsClock;
  BYTE latencyFields;
};
#pragma pack(pop)

static_assert(sizeof(EdidLe16) == 2, "Unexpected EDID little-endian word size");
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
static_assert(sizeof(CtaHdmiForumVendorSpecificDataBlock) == 8,
  "Unexpected HDMI Forum VSDB size");
static_assert(sizeof(CtaHdmiVendorSpecificDataBlock) == 9,
  "Unexpected HDMI VSDB size");
static_assert(CTA_HEADER_SIZE +
  sizeof(CtaHdrStaticMetadataDataBlock) +
  sizeof(CtaColorimetryDataBlock) +
  sizeof(CtaHdmiForumVendorSpecificDataBlock) +
  sizeof(CtaHdmiVendorSpecificDataBlock) <= EDID_BLOCK_SIZE - 1,
  "CTA data blocks exceed extension block space");

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

static void InitEdidBaseBlock(EdidBaseBlock& base)
{
  memcpy(base.header, EDID_HEADER, sizeof(base.header));

  // Manufacturer ID: LGD, product/serial values are arbitrary.
  SetLe16(base.manufacturerId, EDID_MANUFACTURER_ID_LGD);
  SetLe16(base.productCode, EDID_PRODUCT_CODE);
  memcpy(base.serialNumber, EDID_SERIAL_NUMBER, sizeof(base.serialNumber));

  base.manufactureWeek = EDID_MANUFACTURE_WEEK;
  base.manufactureYear = EDID_MANUFACTURE_YEAR_2026;
  base.version         = EDID_VERSION;
  base.revision        = EDID_REVISION;

  base.videoInputDefinition = EDID_VIDEO_INPUT_DIGITAL_10BPC_HDMI_A;
  base.horizontalSizeCm     = EDID_HORIZONTAL_SIZE_CM;
  base.verticalSizeCm       = EDID_VERTICAL_SIZE_CM;
  base.displayGamma         = EDID_DISPLAY_GAMMA_2_2;
  base.supportedFeatures    = EDID_FEATURES_PREFERRED_TIMING_RGB;

  for (UINT i = 0; i < EDID_STANDARD_TIMING_COUNT; ++i)
    base.standardTimings[i] = MakeUnusedStandardTiming();

  base.extensionBlockCount = 1;
}

static bool MakeDetailedTiming(
  EdidDetailedTimingDescriptor& timing,
  const CSettings::DisplayMode& mode)
{
  memset(&timing, 0, sizeof(timing));

  const DWORD hActive = mode.width;
  const DWORD vActive = mode.height;
  const DWORD refresh = mode.refresh;

  if (hActive == 0 || vActive == 0 || refresh == 0 ||
    hActive > 4095 || vActive > 4095)
    return false;

  DWORD hBlank = std::max<DWORD>(160, ((hActive / 20) + 7) & ~7UL);
  DWORD vBlank = std::max<DWORD>(30, vActive / 20);
  if (hBlank > 4095 || vBlank > 4095)
    return false;

  DWORD hSync = std::max<DWORD>(32, hActive / 100);
  hSync = (hSync + 7) & ~7UL;

  DWORD hFront = std::max<DWORD>(48, hBlank / 3);
  hFront = (hFront + 7) & ~7UL;

  if (hFront + hSync >= hBlank)
  {
    hFront = 48;
    hSync = 32;
  }

  const DWORD vFront = 3;
  const DWORD vSync = 5;
  if (vFront + vSync >= vBlank)
    return false;

  const UINT64 pixelClock = (UINT64)(hActive + hBlank) *
    (UINT64)(vActive + vBlank) * (UINT64)refresh;
  const UINT64 pixelClock10KHz = (pixelClock + 5000) / 10000;
  if (pixelClock10KHz == 0 || pixelClock10KHz > 0xffff)
    return false;

  SetLe16(timing.pixelClock10KHz, (DWORD)pixelClock10KHz);

  timing.hActiveLo      = Lo8(hActive);
  timing.hBlankLo       = Lo8(hBlank);
  timing.hActiveBlankHi = PackMsbNibbles(hActive, hBlank);

  timing.vActiveLo      = Lo8(vActive);
  timing.vBlankLo       = Lo8(vBlank);
  timing.vActiveBlankHi = PackMsbNibbles(vActive, vBlank);

  timing.hFrontPorchLo               = Lo8(hFront);
  timing.hSyncPulseWidthLo           = Lo8(hSync);
  timing.vFrontPorchSyncPulseWidthLo = PackLowNibbles(vFront, vSync);
  timing.syncPorchPulseWidthHi       = PackSyncPorchPulseWidthHi(hFront, hSync, vFront, vSync);

  timing.imageWidthMmLo  = Lo8(EDID_IMAGE_WIDTH_MM);
  timing.imageHeightMmLo = Lo8(EDID_IMAGE_HEIGHT_MM);
  timing.imageSizeMmHi   = PackMsbNibbles(EDID_IMAGE_WIDTH_MM, EDID_IMAGE_HEIGHT_MM);

  timing.flags = EDID_DTD_FLAGS_DIGITAL_SEPARATE_SYNC_POSITIVE;
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
    CTA_HDR_EOTF_SMPTE_ST_2084 |
    CTA_HDR_EOTF_HLG);

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
  block.extendedTag = CTA_EXTENDED_TAG_COLORIMETRY;
  block.colorimetry = (BYTE)(CTA_COLORIMETRY_OPYCC_601 |
    CTA_COLORIMETRY_OPRGB      |
    CTA_COLORIMETRY_BT2020_YCC |
    CTA_COLORIMETRY_BT2020_RGB);
  block.metadataAndAdditionalColorimetry = CTA_COLORIMETRY_DCI_P3;
  return block;
}

static CtaHdmiForumVendorSpecificDataBlock MakeCtaHdmiForumVendorSpecificDataBlock()
{
  CtaHdmiForumVendorSpecificDataBlock block = {};

  block.header = MakeCtaDataBlockHeader(CTA_DATA_BLOCK_TAG_VENDOR_SPECIFIC,
    (UINT)(sizeof(block) - sizeof(block.header)));
  memcpy(block.ieeeOui, CTA_HDMI_FORUM_OUI, sizeof(block.ieeeOui));
  block.version              = CTA_HDMI_FORUM_VERSION_1;
  block.maxTmdsCharacterRate = CTA_HDMI_FORUM_MAX_TMDS_CHARACTER_RATE;
  block.flags                = CTA_HDMI_FORUM_SCDC_PRESENT;
  block.reserved             = 0x00;
  return block;
}

static CtaHdmiVendorSpecificDataBlock MakeCtaHdmiVendorSpecificDataBlock()
{
  CtaHdmiVendorSpecificDataBlock block = {};

  block.header = MakeCtaDataBlockHeader(CTA_DATA_BLOCK_TAG_VENDOR_SPECIFIC,
    (UINT)(sizeof(block) - sizeof(block.header)));
  memcpy(block.ieeeOui, CTA_HDMI_LICENSING_OUI, sizeof(block.ieeeOui));
  block.physicalAddressAB = CTA_HDMI_PHYSICAL_ADDRESS_A_B;
  block.physicalAddressCD = CTA_HDMI_PHYSICAL_ADDRESS_C_D;
  block.flags             = CTA_HDMI_DEEP_COLOR_30_36;
  block.maxTmdsClock      = CTA_HDMI_MAX_TMDS_CLOCK_UNSPECIFIED;
  block.latencyFields     = CTA_HDMI_LATENCY_FIELDS_NONE;
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

void CEdid::Build(const CSettings::DisplayModes& modes)
{
  m_data.assign(static_cast<std::vector<BYTE, std::allocator<BYTE>>::size_type>(EDID_BLOCK_SIZE) * 2, 0);

  EdidBaseBlock baseBlock = {};
  InitEdidBaseBlock(baseBlock);

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
  AppendCtaDataBlock(cta, dataOffset, MakeCtaHdrStaticMetadataDataBlock      ());
  AppendCtaDataBlock(cta, dataOffset, MakeCtaColorimetryDataBlock            ());
  AppendCtaDataBlock(cta, dataOffset, MakeCtaHdmiForumVendorSpecificDataBlock());
  AppendCtaDataBlock(cta, dataOffset, MakeCtaHdmiVendorSpecificDataBlock     ());

  ctaBlock.dtdOffset = (BYTE)dataOffset;
  ctaBlock.flags     = 0x00;

  UINT ctaDtdWrite = dataOffset;
  for (; modeIndex < sorted.size() &&
    ctaDtdWrite + EDID_DTD_SIZE <= EDID_BLOCK_SIZE - 1;
    ++modeIndex)
  {
    if (WriteDetailedTiming(cta + ctaDtdWrite, sorted[modeIndex]))
      ctaDtdWrite += EDID_DTD_SIZE;
  }

  SetChecksum(cta);
  memcpy(m_data.data() + sizeof(baseBlock), &ctaBlock, sizeof(ctaBlock));
}