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

#include "util.h"
#include "main.h"

#include "common/debug.h"
#include "common/stringutils.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <fontconfig/fontconfig.h>

static FcConfig * FontConfig = NULL;

bool util_fileGetContents(const char * filename, char ** buffer, size_t * length)
{
  FILE * fh = fopen(filename, "r");
  if (!fh)
  {
    DEBUG_ERROR("Failed to open the file: %s", filename);
    return false;
  }

  if (fseek(fh, 0, SEEK_END) != 0)
  {
    DEBUG_ERROR("Failed to seek");
    fclose(fh);
    return false;
  }

  long fsize = ftell(fh);
  if (fseek(fh, 0, SEEK_SET) != 0)
  {
    DEBUG_ERROR("Failed to seek");
    fclose(fh);
    return false;
  }

  *buffer = malloc(fsize + 1);
  if (!*buffer)
  {
    DEBUG_ERROR("Failed to allocate buffer of %lu bytes", fsize + 1);
    fclose(fh);
    return false;
  }

  if (fread(*buffer, 1, fsize, fh) != fsize)
  {
    DEBUG_ERROR("Failed to read the entire file");
    fclose(fh);
    free(*buffer);
    return false;
  }

  fclose(fh);
  buffer[fsize] = 0;
  *length = fsize;
  return true;
}

void util_cursorToInt(double ex, double ey, int *x, int *y)
{
  /* only smooth if enabled and not using raw mode */
  if (g_params.mouseSmoothing && !(g_cursor.grab && g_params.rawMouse))
  {
    static struct DoublePoint last = { 0 };

    /* only apply smoothing to small deltas */
    if (fabs(ex - last.x) < 5.0 && fabs(ey - last.y) < 5.0)
    {
      ex = last.x = (last.x + ex) / 2.0;
      ey = last.y = (last.y + ey) / 2.0;
    }
    else
    {
      last.x = ex;
      last.y = ey;
    }
  }

  /* convert to int accumulating the fractional error */
  ex += g_cursor.acc.x;
  ey += g_cursor.acc.y;
  g_cursor.acc.x = modf(ex, &ex);
  g_cursor.acc.y = modf(ey, &ey);

  *x = (int)ex;
  *y = (int)ey;
}

bool util_guestCurToLocal(struct DoublePoint *local)
{
  if (!g_cursor.guest.valid || !g_state.posInfoValid)
    return false;

  const struct DoublePoint point =
  {
    .x = g_cursor.guest.x + g_cursor.guest.hx,
    .y = g_cursor.guest.y + g_cursor.guest.hy
  };

  switch((g_state.rotate + g_params.winRotate) % LG_ROTATE_MAX)
  {
    case LG_ROTATE_0:
      local->x = (point.x / g_cursor.scale.x) + g_state.dstRect.x;
      local->y = (point.y / g_cursor.scale.y) + g_state.dstRect.y;;
      break;

    case LG_ROTATE_90:
      local->x = (g_state.dstRect.x + g_state.dstRect.w) -
        point.y / g_cursor.scale.y;
      local->y = (point.x / g_cursor.scale.x) + g_state.dstRect.y;
      break;

    case LG_ROTATE_180:
      local->x = (g_state.dstRect.x + g_state.dstRect.w) -
        point.x / g_cursor.scale.x;
      local->y = (g_state.dstRect.y + g_state.dstRect.h) -
        point.y / g_cursor.scale.y;
      break;

    case LG_ROTATE_270:
      local->x = (point.y / g_cursor.scale.y) + g_state.dstRect.x;
      local->y = (g_state.dstRect.y + g_state.dstRect.h) -
        point.x / g_cursor.scale.x;
      break;
  }

  return true;
}

void util_localCurToGuest(struct DoublePoint *guest)
{
  const struct DoublePoint point =
    g_cursor.pos;

  switch((g_state.rotate + g_params.winRotate) % LG_ROTATE_MAX)
  {
    case LG_ROTATE_0:
      guest->x = (point.x - g_state.dstRect.x) * g_cursor.scale.x;
      guest->y = (point.y - g_state.dstRect.y) * g_cursor.scale.y;
      break;

    case LG_ROTATE_90:
      guest->x = (point.y - g_state.dstRect.y) * g_cursor.scale.y;
      guest->y = (g_state.dstRect.w - point.x + g_state.dstRect.x)
        * g_cursor.scale.x;
      break;

    case LG_ROTATE_180:
      guest->x = (g_state.dstRect.w - point.x + g_state.dstRect.x)
        * g_cursor.scale.x;
      guest->y = (g_state.dstRect.h - point.y + g_state.dstRect.y)
        * g_cursor.scale.y;
      break;

    case LG_ROTATE_270:
      guest->x = (g_state.dstRect.h - point.y + g_state.dstRect.y)
        * g_cursor.scale.y;
      guest->y = (point.x - g_state.dstRect.x) * g_cursor.scale.x;
      break;

    default:
      DEBUG_UNREACHABLE();
  }
}

void util_rotatePoint(struct DoublePoint *point)
{
  double temp;

  switch((g_state.rotate + g_params.winRotate) % LG_ROTATE_MAX)
  {
    case LG_ROTATE_0:
      break;

    case LG_ROTATE_90:
      temp = point->x;
      point->x =  point->y;
      point->y = -temp;
      break;

    case LG_ROTATE_180:
      point->x = -point->x;
      point->y = -point->y;
      break;

    case LG_ROTATE_270:
      temp = point->x;
      point->x = -point->y;
      point->y =  temp;
      break;
  }
}

bool util_hasGLExt(const char * exts, const char * ext)
{
  return str_containsValue(exts, ' ', ext);
}

bool util_initUIFonts(void)
{
  if (FontConfig)
    return true;

  FontConfig = FcInitLoadConfigAndFonts();

  if (!FontConfig)
  {
    DEBUG_ERROR("FcInitLoadConfigAndFonts Failed");
    return false;
  }

  return true;
}

char * util_getUIFont(const char * fontName)
{
  char * ttf = NULL;

  FcPattern * pat = FcNameParse((const FcChar8*) fontName);
  if (!pat)
  {
    DEBUG_ERROR("FCNameParse failed");
    return NULL;
  }

  FcConfigSubstitute(FontConfig, pat, FcMatchPattern);
  FcDefaultSubstitute(pat);
  FcResult result;
  FcChar8 * file = NULL;
  FcPattern * match = FcFontMatch(FontConfig, pat, &result);

  if (!match)
  {
    DEBUG_ERROR("FcFontMatch Failed");
    goto fail_parse;
  }

  if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch)
    ttf = strdup((char *) file);
  else
    DEBUG_ERROR("Failed to locate the requested font: %s", fontName);

  FcPatternDestroy(match);

fail_parse:
  FcPatternDestroy(pat);
  return ttf;
}

void util_freeUIFonts(void)
{
  if (!FontConfig)
    return;

  FcConfigDestroy(FontConfig);
  FontConfig = NULL;
  FcFini();
}
