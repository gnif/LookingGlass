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
static const UINT EDID_DTD_SIZE = 18;

void CEdid::SetChecksum(BYTE* block)
{
  BYTE sum = 0;
  for (UINT i = 0; i < EDID_BLOCK_SIZE - 1; ++i)
    sum = (BYTE)(sum + block[i]);
  block[EDID_BLOCK_SIZE - 1] = (BYTE)(0 - sum);
}

void CEdid::WriteMonitorName(BYTE* desc, const char* name)
{
  memset(desc, 0, EDID_DTD_SIZE);
  desc[3] = 0xfc;
  desc[4] = 0x00;

  UINT i = 0;
  for (; i < 13 && name[i]; ++i)
    desc[5 + i] = (BYTE)name[i];
  if (i < 13)
    desc[5 + i++] = 0x0a;
  for (; i < 13; ++i)
    desc[5 + i] = 0x20;
}

bool CEdid::WriteDetailedTiming(BYTE* dtd, const CSettings::DisplayMode& mode)
{
  memset(dtd, 0, EDID_DTD_SIZE);

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

  dtd[0] = (BYTE)(pixelClock10KHz & 0xff);
  dtd[1] = (BYTE)((pixelClock10KHz >> 8) & 0xff);
  dtd[2] = (BYTE)(hActive & 0xff);
  dtd[3] = (BYTE)(hBlank & 0xff);
  dtd[4] = (BYTE)(((hActive >> 8) & 0x0f) << 4 | ((hBlank >> 8) & 0x0f));
  dtd[5] = (BYTE)(vActive & 0xff);
  dtd[6] = (BYTE)(vBlank & 0xff);
  dtd[7] = (BYTE)(((vActive >> 8) & 0x0f) << 4 | ((vBlank >> 8) & 0x0f));
  dtd[8] = (BYTE)(hFront & 0xff);
  dtd[9] = (BYTE)(hSync & 0xff);
  dtd[10] = (BYTE)((vFront & 0x0f) << 4 | (vSync & 0x0f));
  dtd[11] = (BYTE)(((hFront >> 8) & 0x03) << 6 |
    (((hSync >> 8) & 0x03) << 4) |
    (((vFront >> 4) & 0x03) << 2) |
    ((vSync >> 4) & 0x03));

  // 52cm x 29cm is a conventional 16:9 desktop monitor size.
  const DWORD imageWidth = 520;
  const DWORD imageHeight = 290;
  dtd[12] = (BYTE)(imageWidth & 0xff);
  dtd[13] = (BYTE)(imageHeight & 0xff);
  dtd[14] = (BYTE)(((imageWidth >> 8) & 0x0f) << 4 | ((imageHeight >> 8) & 0x0f));
  dtd[17] = 0x1e; // digital separate sync, positive hsync/vsync
  return true;
}

void CEdid::Build(const CSettings::DisplayModes& modes)
{
  m_data.assign(EDID_BLOCK_SIZE * 2, 0);
  BYTE* base = m_data.data();
  BYTE* cta = base + EDID_BLOCK_SIZE;

  static const BYTE header[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
  memcpy(base, header, sizeof(header));

  // Manufacturer ID: LGD (5-bit EISA ID), product/serial values are arbitrary.
  base[8] = 0x30;
  base[9] = 0xe4;
  base[10] = 0xdd;
  base[11] = 0x1d;
  base[12] = 0x01;
  base[16] = 1;
  base[17] = 36; // 1990 + 36 = 2026
  base[18] = 1;
  base[19] = 4;
  base[20] = 0xb2; // digital input, 10-bit HDMI-A
  base[21] = 52;
  base[22] = 29;
  base[23] = 0x78; // gamma 2.2
  base[24] = 0x0a; // preferred timing, RGB color

  for (UINT i = 38; i < 54; i += 2)
  {
    base[i] = 0x01;
    base[i + 1] = 0x01;
  }

  CSettings::DisplayModes sorted = modes;
  std::stable_sort(sorted.begin(), sorted.end(), [](const CSettings::DisplayMode& a, const CSettings::DisplayMode& b)
    {
      if (a.preferred != b.preferred)
        return a.preferred && !b.preferred;
      if (a.width != b.width)
        return a.width > b.width;
      if (a.height != b.height)
        return a.height > b.height;
      return a.refresh > b.refresh;
    });

  UINT modeIndex = 0;
  UINT dtdOffset = 54;
  for (; modeIndex < sorted.size() && dtdOffset < 54 + EDID_DTD_SIZE * 3; ++modeIndex)
  {
    if (WriteDetailedTiming(base + dtdOffset, sorted[modeIndex]))
      dtdOffset += EDID_DTD_SIZE;
  }
  WriteMonitorName(base + 54 + EDID_DTD_SIZE * 3, "Looking Glass");

  base[126] = 1;
  SetChecksum(base);

  cta[0] = 0x02; // CTA-861 extension
  cta[1] = 0x03;

  UINT dataOffset = 4;
  // CTA HDR Static Metadata data block: HDR, PQ and HLG with type 1 metadata and luminance data.
  cta[dataOffset++] = (7 << 5) | 6;
  cta[dataOffset++] = 0x06;
  cta[dataOffset++] = 0x0d;
  cta[dataOffset++] = 0x01;
  cta[dataOffset++] = 0xa2;
  cta[dataOffset++] = 0xa2;
  cta[dataOffset++] = 0x10;

  // CTA extended colorimetry data block: advertise BT.2020 and DCI-P3 colorimetry.
  cta[dataOffset++] = (7 << 5) | 3;
  cta[dataOffset++] = 0x05;
  cta[dataOffset++] = 0xd8;
  cta[dataOffset++] = 0x00;

  // HDMI Forum vendor-specific data block.
  cta[dataOffset++] = (3 << 5) | 7;
  cta[dataOffset++] = 0xd8;
  cta[dataOffset++] = 0x5d;
  cta[dataOffset++] = 0xc4;
  cta[dataOffset++] = 0x01;
  cta[dataOffset++] = 0x6e;
  cta[dataOffset++] = 0x80;
  cta[dataOffset++] = 0x00;

  // HDMI vendor-specific data block.
  cta[dataOffset++] = (3 << 5) | 8;
  cta[dataOffset++] = 0x03;
  cta[dataOffset++] = 0x0c;
  cta[dataOffset++] = 0x00;
  cta[dataOffset++] = 0x00;
  cta[dataOffset++] = 0x00;
  cta[dataOffset++] = 0x30;
  cta[dataOffset++] = 0x00;
  cta[dataOffset++] = 0x0b;

  UINT ctaDtdOffset = dataOffset;
  if (ctaDtdOffset < 4)
    ctaDtdOffset = 4;

  UINT ctaDtdWrite = ctaDtdOffset;
  for (; modeIndex < sorted.size() && ctaDtdWrite + EDID_DTD_SIZE <= EDID_BLOCK_SIZE - 1; ++modeIndex)
  {
    if (WriteDetailedTiming(cta + ctaDtdWrite, sorted[modeIndex]))
      ctaDtdWrite += EDID_DTD_SIZE;
  }

  cta[2] = (BYTE)ctaDtdOffset;
  cta[3] = 0x00;
  SetChecksum(cta);
}
