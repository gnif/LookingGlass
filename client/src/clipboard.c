/**
 * Looking Glass
 * Copyright © 2017-2021 The Looking Glass Authors
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

#include "clipboard.h"

#include "main.h"
#include "ll.h"

#include "common/debug.h"

LG_ClipboardData cb_spiceTypeToLGType(const SpiceDataType type)
{
  switch(type)
  {
    case SPICE_DATA_TEXT: return LG_CLIPBOARD_DATA_TEXT; break;
    case SPICE_DATA_PNG : return LG_CLIPBOARD_DATA_PNG ; break;
    case SPICE_DATA_BMP : return LG_CLIPBOARD_DATA_BMP ; break;
    case SPICE_DATA_TIFF: return LG_CLIPBOARD_DATA_TIFF; break;
    case SPICE_DATA_JPEG: return LG_CLIPBOARD_DATA_JPEG; break;
    default:
      DEBUG_ERROR("invalid spice data type");
      return LG_CLIPBOARD_DATA_NONE;
  }
}

SpiceDataType cb_lgTypeToSpiceType(const LG_ClipboardData type)
{
  switch(type)
  {
    case LG_CLIPBOARD_DATA_TEXT: return SPICE_DATA_TEXT; break;
    case LG_CLIPBOARD_DATA_PNG : return SPICE_DATA_PNG ; break;
    case LG_CLIPBOARD_DATA_BMP : return SPICE_DATA_BMP ; break;
    case LG_CLIPBOARD_DATA_TIFF: return SPICE_DATA_TIFF; break;
    case LG_CLIPBOARD_DATA_JPEG: return SPICE_DATA_JPEG; break;
    default:
      DEBUG_ERROR("invalid clipboard data type");
      return SPICE_DATA_NONE;
  }
}

void cb_spiceNotice(const SpiceDataType type)
{
  if (!g_params.clipboardToLocal)
    return;

  if (!g_state.cbAvailable)
    return;

  g_state.cbType = type;
  g_state.ds->cbNotice(cb_spiceTypeToLGType(type));
}

void cb_spiceData(const SpiceDataType type, uint8_t * buffer, uint32_t size)
{
  if (!g_params.clipboardToLocal)
    return;

  if (type == SPICE_DATA_TEXT)
  {
    // dos2unix
    uint8_t  * p       = buffer;
    uint32_t   newSize = size;
    for(uint32_t i = 0; i < size; ++i)
    {
      uint8_t c = buffer[i];
      if (c == '\r')
      {
        --newSize;
        continue;
      }
      *p++ = c;
    }
    size = newSize;
  }

  struct CBRequest * cbr;
  if (ll_shift(g_state.cbRequestList, (void **)&cbr))
  {
    cbr->replyFn(cbr->opaque, cb_spiceTypeToLGType(type), buffer, size);
    free(cbr);
  }
}

void cb_spiceRelease(void)
{
  if (!g_params.clipboardToLocal)
    return;

  if (g_state.cbAvailable)
    g_state.ds->cbRelease();
}

void cb_spiceRequest(const SpiceDataType type)
{
  if (!g_params.clipboardToVM)
    return;

  if (g_state.cbAvailable)
    g_state.ds->cbRequest(cb_spiceTypeToLGType(type));
}
